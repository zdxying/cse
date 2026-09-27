#include "ur_emit.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backend/codegen.h"
#include "config.h"
#include "frontend/ast.h"
#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "lattice_resolve.h"
#include "passes/counter_prop.h"
#include "passes/pass_manager.h"

namespace cse {
namespace freelb {
namespace {

struct LatSetDef {
  const char* name;
  int d;
  int q;
};


const LatSetDef kLatsets[] = {
    {"D2Q5", 2, 5},   {"D2Q9", 2, 9},   {"D3Q7", 3, 7},
    {"D3Q15", 3, 15}, {"D3Q19", 3, 19}, {"D3Q27", 3, 27},
};

std::string baseName(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Accept `// @cse`, `//  @cse` and `//@cse` uniformly.
std::string normalizeMarkers(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  std::istringstream iss(src);
  std::string line;
  bool first = true;
  while (std::getline(iss, line)) {
    size_t p = line.find("//");
    if (p != std::string::npos) {
      size_t q = p + 2;
      while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
      if (line.compare(q, 4, "@cse") == 0) {
        line = line.substr(0, p) + "//@cse";
      }
    }
    if (!first) out += "\n";
    first = false;
    out += line;
  }
  return out;
}

// Copy the leading C-style license block, if any.
std::string extractLicense(const std::string& src) {
  if (src.size() >= 2 && src[0] == '/' && src[1] == '*') {
    size_t pos = src.find("*/");
    if (pos != std::string::npos) return src.substr(0, pos + 2) + "\n";
  }
  return "";
}

std::string stripModifiers(std::string type) {
  for (;;) {
    if (type.rfind("static ", 0) == 0) {
      type = type.substr(7);
      continue;
    }
    if (type.rfind("inline ", 0) == 0) {
      type = type.substr(7);
      continue;
    }
    break;
  }
  return type;
}

std::string replaceAll(std::string s, const std::string& from,
                       const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

enum class StructKind { Cell, CellType, TLatSet, TLatSetD, Unsupported };

// Classify a marked struct by its template parameter list:
//   <typename CELL>                              -> Cell
//   <typename CELLTYPE, <extra>...>              -> CellType
//   <typename T, typename LatSet>                -> TLatSet
//   <typename T, typename LatSet, <nontype>>     -> TLatSetD
StructKind classifyStruct(const StructDef& sd, std::string& nonTypeName) {
  const auto& tp = sd.templateParams;
  if (tp.empty() || !tp[0].isType) return StructKind::Unsupported;
  if (tp[0].paramName == "CELL") return StructKind::Cell;
  if (tp[0].paramName == "CELLTYPE") return StructKind::CellType;
  if (tp.size() == 3 && tp[1].isType && !tp[2].isType) {
    nonTypeName = tp[2].paramName;
    return StructKind::TLatSetD;
  }
  if (tp.size() == 2 && tp[1].isType) return StructKind::TLatSet;
  return StructKind::Unsupported;
}

std::string emitMethod(const FunctionDef& method, const std::string& body) {
  std::string out;
  out += "  __any__ static " + stripModifiers(method.returnType) + " " +
         method.name + "(";
  for (size_t i = 0; i < method.params.size(); ++i) {
    if (i) out += ", ";
    out += method.params[i].type + " " + method.params[i].name;
  }
  out += "){\n";
  out += body;
  out += "  }\n";
  return out;
}

}  // namespace

UrConfig detectUrConfig(const std::string& inputPath) {
  std::string name = baseName(inputPath);
  UrConfig cfg;
  if (name == "moment.h") {
    cfg.include = "lbm/moment.h";
    cfg.ns = "moment";
  } else if (name == "equilibrium.h") {
    cfg.include = "lbm/equilibrium.h";
    cfg.ns = "equilibrium";
  } else if (name == "force.h") {
    cfg.include = "lbm/force.h";
    cfg.ns = "force";
  } else {
    throw std::runtime_error("unknown input file: " + name);
  }
  return cfg;
}

bool generateUrHeader(const std::string& inputPath,
                      const std::string& outputPath,
                      const UrGenerateOptions& opts) {
  auto wantLattice = [&opts](const char* name) {
    if (opts.lattices.empty()) return true;
    for (const auto& l : opts.lattices)
      if (l == name) return true;
    return false;
  };
  std::ifstream ifs(inputPath);
  if (!ifs.is_open()) {
    std::cerr << "csegen: cannot open " << inputPath << "\n";
    return false;
  }
  std::string source((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
  ifs.close();

  UrConfig cfg = detectUrConfig(inputPath);
  std::string normalized = normalizeMarkers(source);
  auto regions = RegionExtractor().extract(normalized);
  if (regions.empty()) {
    std::cerr << "csegen: no // @cse markers in " << inputPath << "\n";
    return false;
  }

  std::string out;
  out += extractLicense(source);
  out += "// " + baseName(outputPath) + " fragment generated by csegen from " +
         cfg.include + "\n";
  out += "#pragma once\n";
  out += "#include \"" + cfg.include + "\"\n";
  out += "#ifdef _UNROLLFOR\n";
  out += "namespace " + cfg.ns + " {\n\n";
  // The POP storage strategy (cudev::DirectPop / cudev::RegPop) is a trailing
  // template parameter of the cell, and the specialisations below are emitted
  // generic over it.  Both branches therefore take the same four parameters so
  // that one piece of emitted text parses in the host pass and in the device
  // pass alike; the host alias accepts POPPOLICY and ignores it, which leaves it
  // a free (unused) template parameter of the specialisation there.
  out += "#ifdef __CUDA_ARCH__\n";
  out += "template <typename T, typename LatSet, typename TypePack, typename POPPOLICY>\n";
  out += "using CELL = cudev::Cell<T, LatSet, TypePack, POPPOLICY>;\n";
  out += "#else\n";
  out += "template <typename T, typename LatSet, typename TypePack, typename POPPOLICY>\n";
  out += "using CELL = Cell<T, LatSet, TypePack>;\n";
  out += "#endif\n\n";
  // In the host pass the alias above drops POPPOLICY, so nvcc emits one
  // "template parameter POPPOLICY is not used in or cannot be deduced" (#842-D)
  // per specialisation below -- 72 of them for the three bases, drowning any
  // real diagnostic.  It is inherent to sharing one piece of emitted text
  // between the two passes, so the warning is suppressed for exactly this file
  // and re-enabled at the end of it.  g++ has no equivalent diagnostic, hence
  // the __NVCC__ guard.
  out += "#ifdef __NVCC__\n";
  out += "#pragma nv_diag_suppress 842\n";
  out += "#endif\n";

  int structCount = 0;
  int latCount = 0;
  for (const auto& l : kLatsets)
    if (wantLattice(l.name)) ++latCount;

  for (const auto& region : regions) {
    CSEConfig parseCfg = createFreeLBConfig();
    Parser::ParseResult parsed;
    try {
      Lexer lexer(region.code, parseCfg);
      std::vector<Token> tokens = lexer.tokenize();
      Parser parser(tokens, parseCfg);
      parsed = parser.parseAll();
    } catch (const std::exception& e) {
      std::cerr << "csegen: parse error: " << e.what() << "\n";
      return false;
    }

    for (const auto& sdPtr : parsed.structDefs) {
      const StructDef& sd = *sdPtr;
      std::string nonType;
      StructKind kind = classifyStruct(sd, nonType);
      if (kind == StructKind::Unsupported) {
        std::cerr << "csegen: skipping unsupported struct " << sd.name << "\n";
        continue;
      }

      for (const auto& lat : kLatsets) {
        if (!wantLattice(lat.name)) continue;
        LatticeConfig latCfg{"LatSet", lat.name, lat.d, lat.q, 1.0 / 3.0};
        CSEConfig base = createFreeLBConfig(latCfg);
        // Only the force/moment shapes need Vector lowering; keep the
        // equilibrium (CELL) path on the existing lattice-resolve route.
        base.lowerVectors = (kind != StructKind::Cell);

        auto emitOne = [&](const std::string& header, const std::string& aliases,
                           double dVal, bool bindD) {
          CSEConfig cfg2 = base;
          if (bindD) cfg2.constBindings[nonType] = dVal;

          std::string methods;
          for (const auto& method : sd.methods) {
            IRModule module;
            IRBuilder builder(&module, cfg2);
            builder.buildFunction(*method);
            CostResult before;
            if (opts.report) before = analyzeCost(module, &cfg2);
            std::unique_ptr<Pass> counterProp;
            if (cfg2.lowerVectors) {
              counterProp = createCounterPropPass(cfg2.vectorLocalName);
            }
            auto pm = PassManager::createDefault(
                cfg2, false, createLatticeResolvePass("LatSet", lat.name),
                std::move(counterProp));
            pm.runAll(module);
            if (opts.report) {
              UrCostEntry e;
              e.lattice = lat.name;
              e.function = sd.name + "::" + method->name;
              e.component = bindD ? static_cast<int>(dVal) : -1;
              e.before = before;
              e.after = analyzeCost(module, &cfg2);
              opts.report->entries.push_back(std::move(e));
            }
            CodeGen codegen;
            std::string body = codegen.generateBody(module, 2);
            body = replaceAll(body, "auto ", "const T ");
            methods += emitMethod(*method, body);
          }

          std::string spec = header;
          spec += aliases;
          if (kind == StructKind::CellType &&
              methods.find("GenericRho") != std::string::npos) {
            spec += "using GenericRho = typename CELLTYPE::GenericRho;\n";
          }
          // For CellType structs, rewrite method signatures to use CELLTYPE
          // instead of CELL (the source uses `using CELL = CELLTYPE;` internally)
          if (kind == StructKind::CellType) {
            methods = replaceAll(methods, " CELL&", " CELLTYPE&");
            methods = replaceAll(methods, "CELL&", "CELLTYPE&");
            methods = replaceAll(methods, " CELL ", " CELLTYPE ");
            methods = replaceAll(methods, "CELL,", "CELLTYPE,");
            methods = replaceAll(methods, "CELL)", "CELLTYPE)");
          }
          spec += methods;
          spec += "};\n\n";
          out += spec;
        };

        const std::string latT = std::string(lat.name) + "<T>";
        // POPPOLICY is forwarded into the specialisation so that one emitted
        // specialisation covers every cell storage strategy (cudev::DirectPop,
        // cudev::RegPop, ...).  It MUST stay last in the pattern: the CELL alias
        // takes <T, LatSet, TypePack, POPPOLICY>, so anything else silently
        // fails to match and falls back to the loop-based primary template.
        if (kind == StructKind::Cell) {
          std::string header =
              "template <typename T, typename TypePack, typename POPPOLICY>\n";
          header += "struct " + sd.name + "<CELL<T, " + latT +
                    ", TypePack, POPPOLICY>>{\n";
          emitOne(header, "using LatSet = " + latT + ";\n", 0, false);
        } else if (kind == StructKind::CellType) {
          std::string extraDecl, extraArg;
          for (size_t i = 1; i < sd.templateParams.size(); ++i) {
            const auto& tp = sd.templateParams[i];
            extraDecl += ", " + tp.paramType + " " + tp.paramName;
            extraArg += ", " + tp.paramName;
          }
          std::string header = "template <typename T, typename TypePack, typename POPPOLICY" +
                               extraDecl + ">\n";
          header += "struct " + sd.name + "<CELL<T, " + latT +
                    ", TypePack, POPPOLICY>" + extraArg + ">{\n";
          std::string aliases =
              "using CELLTYPE = CELL<T, " + latT + ", TypePack, POPPOLICY>;\n";
          aliases += "using LatSet = " + latT + ";\n";
          emitOne(header, aliases, 0, false);
        } else if (kind == StructKind::TLatSet) {
          std::string header = "template <typename T>\n";
          header += "struct " + sd.name + "<T, " + latT + ">{\n";
          emitOne(header, "using LatSet = " + latT + ";\n", 0, false);
        } else {  // TLatSetD: one specialization per component index
          for (int dv = 0; dv < lat.d; ++dv) {
            std::string header = "template <typename T>\n";
            header += "struct " + sd.name + "<T, " + latT + ", " +
                      std::to_string(dv) + ">{\n";
            emitOne(header, "using LatSet = " + latT + ";\n", dv, true);
          }
        }
      }
      structCount++;
    }
  }

  out += "}  // namespace " + cfg.ns + "\n";
  out += "#ifdef __NVCC__\n";
  out += "#pragma nv_diag_default 842\n";
  out += "#endif\n";
  out += "#endif  // _UNROLLFOR\n";

  std::ofstream ofs(outputPath);
  if (!ofs.is_open()) {
    std::cerr << "csegen: cannot write " << outputPath << "\n";
    return false;
  }
  ofs << out;
  std::cout << "csegen: wrote " << outputPath << " (" << structCount
            << " structs x " << latCount << " lattice sets)\n";
  return true;
}

}  // namespace freelb
}  // namespace cse
