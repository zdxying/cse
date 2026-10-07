#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/cost_model.h"
#include "backend/codegen.h"
#include "frontend/diagnostics.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "config.h"
#include "lattice_resolve.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "passes/pass_manager.h"

static void printUsage(const char* prog) {
  std::cerr << "Usage: " << prog << " <input.cpp> [options]\n"
            << "Options:\n"
            << "  -r, --recombine    Enable expression recombination\n"
            << "  -c, --cost         Analyze and report FLOP cost comparison\n"
            << "  -s, --safe         Conservative mode (no unsafe algebraic rules)\n"
            << "  -v, --verbose      Print each pass as it runs (to stderr)\n"
            << "  --json             Output in JSON format (use with -c)\n"
            << "  --lattice NAME     Lattice set for cost model (D2Q5, D2Q9, D3Q7, D3Q15, D3Q19, D3Q27)\n"
            << "  -h, --help         Show this help\n"
            << "\n"
            << "Exit status: 0 if every region was optimized, 2 if some were left\n"
            << "             unchanged because they could not be read, 3 if none could.\n";
}

struct FunctionCost {
  std::string name;
  cse::CostResult before;
  cse::CostResult after;
};

struct OptResult {
  std::string code;
  cse::CostResult costBefore;
  cse::CostResult costAfter;
  std::vector<FunctionCost> functionCosts;
};

// Optimize a set of struct definitions and functions and append the generated
// code to `optResult`. Shared by the top level and by namespace bodies.
static void optimizeFunctionsAndStructs(
    const std::vector<std::unique_ptr<cse::FunctionDef>>& funcs,
    const std::vector<std::unique_ptr<cse::StructDef>>& structs,
    const cse::CSEConfig& config, const std::string& latAlias,
    const std::string& latName, bool enableRecombine, bool collectCost,
    bool verbose, OptResult& optResult) {
  // Optimize struct methods (each method gets its own IR pipeline). Pure data
  // structs (no methods) are emitted once via `structPtrs` below.
  std::vector<cse::OptimizedStruct> optStructs;
  for (auto& sd : structs) {
    if (sd->methods.empty()) continue;
    cse::OptimizedStruct os;
    os.def = sd.get();
    for (auto& method : sd->methods) {
      auto methodMod = std::make_unique<cse::IRModule>();
      cse::IRBuilder builder(methodMod.get(), config);
      builder.buildFunction(*method);

      if (collectCost) {
        // Measure "before" cost IMMEDIATELY after IR build, before ANY passes
        auto before = cse::analyzeCost(*methodMod, &config);
        optResult.costBefore += before;
        optResult.functionCosts.push_back({sd->name + "::" + method->name, before, {}});
      }

      auto pm = cse::PassManager::createDefault(
          config, enableRecombine, cse::freelb::createLatticeResolvePass(latAlias, latName));
      pm.runAll(*methodMod, verbose);

      if (collectCost) {
        auto after = cse::analyzeCost(*methodMod, &config);
        optResult.costAfter += after;
        if (!optResult.functionCosts.empty()) {
          optResult.functionCosts.back().after = after;
        }
      }
      os.methodModules.push_back(std::move(methodMod));
    }
    optStructs.push_back(std::move(os));
  }

  // Collect struct definition pointers (for pure data structs)
  std::vector<cse::StructDef*> structPtrs;
  for (auto& sd : structs) {
    if (sd->methods.empty()) {
      structPtrs.push_back(sd.get());
    }
  }

  // Build IR for each function: AST → DAG-based IR
  bool emitStructs = true;
  for (auto& func : funcs) {
    cse::IRModule module;
    cse::IRBuilder builder(&module, config);
    builder.buildFunction(*func);

    if (collectCost) {
      // Measure "before" cost IMMEDIATELY after IR build, before ANY passes
      auto before = cse::analyzeCost(module, &config);
      optResult.costBefore += before;
      optResult.functionCosts.push_back({func->name, before, {}});
    }

    auto pm = cse::PassManager::createDefault(
        config, enableRecombine, cse::freelb::createLatticeResolvePass(latAlias, latName));
    pm.runAll(module, verbose);

    if (collectCost) {
      auto after = cse::analyzeCost(module, &config);
      optResult.costAfter += after;
      if (!optResult.functionCosts.empty()) {
        optResult.functionCosts.back().after = after;
      }
    }

    // Generate code (emit struct defs only before first function)
    cse::CodeGen codegen;
    if (emitStructs) {
      optResult.code +=
          codegen.generate(module, structPtrs, optStructs, func->templateParams);
      emitStructs = false;
    } else {
      optResult.code += codegen.generate(module, {}, {}, func->templateParams);
    }
  }

  // If no functions, emit the structs on their own. This has to include the
  // pure data structs (`structPtrs`): they are otherwise only emitted on the
  // first function iteration below, so a region that marks a data struct and
  // nothing else used to be dropped from the output entirely -- leaving the
  // file referencing a type that no longer exists.
  if (funcs.empty() && (!structPtrs.empty() || !optStructs.empty())) {
    cse::IRModule emptyModule;
    cse::CodeGen codegen;
    optResult.code += codegen.generate(emptyModule, structPtrs, optStructs);
  }
}

// Pipeline: source text → Lexer → Parser → IRBuilder → PassManager → CodeGen
// Each //@cse region is processed independently through this pipeline.
static OptResult optimizeRegion(const std::string& code,
                                const cse::CSEConfig& config,
                                const std::string& latAlias,
                                const std::string& latName,
                                bool enableRecombine, bool collectCost,
                                bool verbose) {
  // 2. Lex: tokenize source
  cse::Lexer lexer(code, config);
  auto tokens = lexer.tokenize();

  // 3. Parse: tokens → AST (FunctionDef, StructDef)
  cse::Parser parser(tokens, config);
  auto result = parser.parseAll();

  if (result.functions.empty() && result.structDefs.empty() &&
      result.usingDecls.empty() && result.namespaces.empty()) {
    OptResult r;
    r.code = code;
    return r;
  }

  OptResult optResult;

  // Top-level using declarations
  for (auto& ud : result.usingDecls) {
    optResult.code += "using " + ud->aliasName + " = " + ud->underlyingType + ";\n";
  }

  // Namespaces: emit using declarations and optimize the contained
  // structs/functions, wrapping the result in the namespace.
  for (auto& ns : result.namespaces) {
    optResult.code += "namespace " + ns->name + " {\n";
    for (auto& ud : ns->usingDecls) {
      optResult.code += "using " + ud->aliasName + " = " + ud->underlyingType + ";\n";
    }
    optimizeFunctionsAndStructs(ns->functions, ns->structDefs, config,
                                latAlias, latName, enableRecombine, collectCost,
                                verbose, optResult);
    optResult.code += "}\n";
  }

  // Top-level structs and functions
  optimizeFunctionsAndStructs(result.functions, result.structDefs, config,
                              latAlias, latName, enableRecombine, collectCost,
                              verbose, optResult);

  return optResult;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::string inputFile;
  std::string latticeName;
  bool enableRecombine = false;
  bool collectCost = false;
  bool outputJson = false;
  bool safeMode = false;
  bool verbose = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "-r" || arg == "--recombine") {
      enableRecombine = true;
    } else if (arg == "-c" || arg == "--cost") {
      collectCost = true;
    } else if (arg == "-s" || arg == "--safe") {
      safeMode = true;
    } else if (arg == "-v" || arg == "--verbose") {
      verbose = true;
    } else if (arg == "--json") {
      outputJson = true;
    } else if (arg == "--lattice") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --lattice requires a name\n";
        printUsage(argv[0]);
        return 1;
      }
      latticeName = argv[++i];
    } else if (arg[0] != '-') {
      inputFile = arg;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      printUsage(argv[0]);
      return 1;
    }
  }

  // Default configuration is FreeLB-flavored (aggressive algebraic rules). The
  // generic/safe mode disables assumptions that are unsafe for arbitrary C++.
  cse::freelb::LatticeConfig latCfg = cse::freelb::createLatticeConfig(latticeName);
  cse::CSEConfig config = cse::freelb::createFreeLBConfig(latCfg);
  if (safeMode) {
    cse::freelb::LatticeConfig safeLat = latCfg;
    cse::CSEConfig safe = cse::freelb::createFreeLBConfig(safeLat);
    safe.tokenFilter = config.tokenFilter;  // keep parsing behavior
    safe.simplifyBraceInit = false;
    safe.assumeNumericCommutative = false;
    safe.assumeNumericAssociative = false;
    safe.allowFpReassoc = false;
    safe.noAlias = false;
    safe.isPureFunction = nullptr;
    config = safe;
  }

  // `-r` is an explicit request for expression recombination, which is itself a
  // floating-point regrouping (`a*x + a*y -> a*(x+y)` rounds differently), so the
  // request carries its own allowFpReassoc licence. Without this, `-s -r` would
  // silently disable the very pass the user asked for, because `-s` clears
  // allowFpReassoc. Only ExprRecombine is affected: the other allowFpReassoc
  // consumers also require `assoc`/`numeric_`, which `-s` keeps off.
  if (enableRecombine) {
    config.allowFpReassoc = true;
  }

  if (inputFile.empty()) {
    std::cerr << "Error: no input file specified\n";
    printUsage(argv[0]);
    return 1;
  }

  // Read input file
  std::ifstream ifs(inputFile);
  if (!ifs.is_open()) {
    std::cerr << "Error: cannot open file " << inputFile << "\n";
    return 1;
  }
  std::string source(
    (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ifs.close();

  // Normalize // @cse markers to //@cse (accepts `// @cse`, `//  @cse`, `//@cse`)
  auto normalizeMarkers = [](std::string& src) {
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
    src.swap(out);
  };
  normalizeMarkers(source);

  // Find CSE regions
  auto regions = cse::RegionExtractor().extract(source);
  if (regions.empty()) {
    std::cerr << "No //@cse markers found in " << inputFile << "\n";
    return 1;
  }

  // Process each region and build output
  std::string output;
  size_t lastEnd = 0;
  std::istringstream iss(source);
  std::string line;
  std::vector<std::string> allLines;
  while (std::getline(iss, line)) {
    allLines.push_back(line);
  }

  cse::CostResult totalBefore, totalAfter;
  size_t skippedRegions = 0;
  std::vector<FunctionCost> allFunctionCosts;

  for (const auto& region : regions) {
    // Copy lines before this region
    for (size_t i = lastEnd; i < region.startLine - 1 && i < allLines.size(); i++) {
      output += allLines[i] + "\n";
    }

    // Copy the //@cse marker line
    if (region.startLine - 1 < allLines.size()) {
      output += allLines[region.startLine - 1] + "\n";
    }

    // Optimize and output the region. A region the frontend cannot read is
    // reported and passed through unchanged while the rest of the file is still
    // optimized: the first parse error used to reach std::terminate, and since
    // the output file is only created at the end, that cost the user *every*
    // region in the file, including the ones already optimized successfully.
    try {
      auto opt = optimizeRegion(region.code, config, latCfg.alias, latCfg.name,
                                enableRecombine, collectCost, verbose);
      output += opt.code;

      if (collectCost) {
        totalBefore += opt.costBefore;
        totalAfter += opt.costAfter;
        allFunctionCosts.insert(allFunctionCosts.end(),
                                opt.functionCosts.begin(),
                                opt.functionCosts.end());
      }
    } catch (const cse::CSEError& e) {
      // The diagnostic's line is relative to the region text; report the line in
      // the file the user handed us (the region body starts one line after its
      // //@cse marker, which is region.startLine).
      const cse::Diagnostic& d = e.diagnostic();
      std::cerr << inputFile << ":" << (region.startLine + d.line) << ":" << d.col
                << ": " << d.phase << " error: " << d.message
                << " -- region passed through unchanged\n";
      output += region.code;
      skippedRegions++;
    } catch (const std::exception& e) {
      std::cerr << inputFile << ":" << region.startLine
                << ": region passed through unchanged: " << e.what() << "\n";
      output += region.code;
      skippedRegions++;
    }

    lastEnd = region.endLine;
  }

  // Copy remaining lines
  for (size_t i = lastEnd; i < allLines.size(); i++) {
    output += allLines[i] + "\n";
  }

  // Write output file
  std::string outputFile = inputFile + ".cse";
  std::ofstream ofs(outputFile);
  if (!ofs.is_open()) {
    std::cerr << "Error: cannot write to " << outputFile << "\n";
    return 1;
  }
  ofs << output;
  ofs.close();

  std::cout << "Optimized output written to " << outputFile << "\n";

  if (skippedRegions > 0) {
    std::cerr << skippedRegions << " of " << regions.size()
              << " regions were left unoptimized\n";
  }

  // Cost analysis output
  if (collectCost) {
    long long saved = totalBefore.savedFlops(totalAfter);
    double pct = totalBefore.savedPercent(totalAfter);

    auto jsonSide = [](const char* tag, const cse::CostResult& r) {
      std::cout << "  \"" << tag << "\": {\"flops\":" << r.flops
                << ",\"vectorOps\":" << r.vectorOps
                << ",\"memOps\":" << r.memOps
                << ",\"nodes\":" << r.totalNodes
                << ",\"stmts\":" << r.stmts << ",\"vars\":" << r.vars
                << ",\"unknownLoops\":" << r.unknownLoops
                << ",\"unmodeledCalls\":" << r.unmodeledCalls << "},\n";
    };

    if (outputJson) {
      std::cout << "{\n";
      jsonSide("before", totalBefore);
      jsonSide("after", totalAfter);
      std::cout << "  \"saved\": {\"flops\":" << saved
                << ",\"percent\":" << std::round(pct * 10.0) / 10.0 << "},\n";
      std::cout << "  \"skipped\": " << skippedRegions << "\n";
      std::cout << "}\n";
    } else {
      std::cout << "\n=== FLOP Cost Analysis ===\n";

      if (!allFunctionCosts.empty()) {
        std::cout << "\nPer-function breakdown:\n";
        std::cout << "  " << std::left << std::setw(35) << "Function"
                  << std::right << std::setw(10) << "Before"
                  << std::setw(10) << "After"
                  << std::setw(10) << "Saved"
                  << std::setw(8) << "%" << "\n";
        std::cout << "  " << std::string(73, '-') << "\n";

        for (const auto& fc : allFunctionCosts) {
          long long fsaved = fc.before.savedFlops(fc.after);
          double fpct = fc.before.savedPercent(fc.after);
          std::cout << "  " << std::left << std::setw(35) << fc.name
                    << std::right << std::setw(10) << fc.before.flops
                    << std::setw(10) << fc.after.flops
                    << std::setw(10) << fsaved
                    << std::setw(7) << std::fixed << std::setprecision(1)
                    << fpct << "%\n";
        }
        std::cout << "  " << std::string(73, '-') << "\n";
      }

      auto printCost = [](const char* label, const cse::CostResult& b,
                          const cse::CostResult& a) {
        std::cout << label << ":\n"
                  << "  Before:  " << b.flops << " flops, "
                  << b.vectorOps << " vector-ops, " << b.memOps << " mem-ops, "
                  << b.totalNodes << " nodes, " << b.stmts << " stmts, "
                  << b.vars << " vars\n"
                  << "  After:   " << a.flops << " flops, "
                  << a.vectorOps << " vector-ops, " << a.memOps << " mem-ops, "
                  << a.totalNodes << " nodes, " << a.stmts << " stmts, "
                  << a.vars << " vars\n";
        long long s = b.savedFlops(a);
        double p = b.savedPercent(a);
        std::cout << "  Saved:   " << s << " flops ("
                  << std::round(p * 10.0) / 10.0 << "%)\n";
      };
      printCost("Total", totalBefore, totalAfter);

      long long covLoops = totalBefore.unknownLoops + totalAfter.unknownLoops;
      long long covCalls =
          totalBefore.unmodeledCalls + totalAfter.unmodeledCalls;
      if (covLoops || covCalls) {
        std::cout << "  Coverage: " << covLoops << " unresolved loops, "
                  << covCalls << " unmodeled calls (flops is a lower bound)\n";
      }
    }
  }

  // 0 = every region optimized, 2 = some skipped, 3 = none could be.
  if (skippedRegions == 0) return 0;
  return skippedRegions == regions.size() ? 3 : 2;
}
