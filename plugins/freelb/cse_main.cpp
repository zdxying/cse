#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/cost_model.h"
#include "backend/codegen.h"
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
            << "  -h, --help         Show this help\n";
}

struct OptResult {
  std::string code;
  cse::CostResult costBefore;
  cse::CostResult costAfter;
};

// Optimize a set of struct definitions and functions and append the generated
// code to `optResult`. Shared by the top level and by namespace bodies.
static void optimizeFunctionsAndStructs(
    const std::vector<std::unique_ptr<cse::FunctionDef>>& funcs,
    const std::vector<std::unique_ptr<cse::StructDef>>& structs,
    const cse::CSEConfig& config, bool enableRecombine, bool collectCost,
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
        auto before = cse::analyzeCost(*methodMod);
        optResult.costBefore.flops += before.flops;
        optResult.costBefore.totalNodes += before.totalNodes;
        optResult.costBefore.stmts += before.stmts;
        optResult.costBefore.vars += before.vars;
      }
      auto pm = cse::PassManager::createDefault(
          config, enableRecombine, cse::freelb::createLatticeResolvePass());
      pm.runAll(*methodMod, verbose);
      if (collectCost) {
        auto after = cse::analyzeCost(*methodMod);
        optResult.costAfter.flops += after.flops;
        optResult.costAfter.totalNodes += after.totalNodes;
        optResult.costAfter.stmts += after.stmts;
        optResult.costAfter.vars += after.vars;
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
      auto before = cse::analyzeCost(module);
      optResult.costBefore.flops += before.flops;
      optResult.costBefore.totalNodes += before.totalNodes;
      optResult.costBefore.stmts += before.stmts;
      optResult.costBefore.vars += before.vars;
    }

    auto pm = cse::PassManager::createDefault(
        config, enableRecombine, cse::freelb::createLatticeResolvePass());
    pm.runAll(module, verbose);

    if (collectCost) {
      auto after = cse::analyzeCost(module);
      optResult.costAfter.flops += after.flops;
      optResult.costAfter.totalNodes += after.totalNodes;
      optResult.costAfter.stmts += after.stmts;
      optResult.costAfter.vars += after.vars;
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

  // If no functions but have structs, emit structs only
  if (funcs.empty() && !optStructs.empty()) {
    cse::IRModule emptyModule;
    cse::CodeGen codegen;
    optResult.code += codegen.generate(emptyModule, structPtrs, optStructs);
  }
}

// Pipeline: source text → Lexer → Parser → IRBuilder → PassManager → CodeGen
// Each //@cse region is processed independently through this pipeline.
static OptResult optimizeRegion(const std::string& code,
                                const cse::CSEConfig& config,
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
                                enableRecombine, collectCost, verbose, optResult);
    optResult.code += "}\n";
  }

  // Top-level structs and functions
  optimizeFunctionsAndStructs(result.functions, result.structDefs, config,
                              enableRecombine, collectCost, verbose, optResult);

  return optResult;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::string inputFile;
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
  cse::CSEConfig config = cse::freelb::createFreeLBConfig();
  if (safeMode) {
    cse::CSEConfig safe;
    safe.tokenFilter = config.tokenFilter;  // keep parsing behavior
    safe.simplifyBraceInit = false;
    safe.assumeNumericCommutative = false;
    safe.assumeNumericAssociative = false;
    safe.allowFpReassoc = false;
    safe.noAlias = false;
    safe.isPureFunction = nullptr;
    config = safe;
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

  for (const auto& region : regions) {
    // Copy lines before this region
    for (size_t i = lastEnd; i < region.startLine - 1 && i < allLines.size(); i++) {
      output += allLines[i] + "\n";
    }

    // Copy the //@cse marker line
    if (region.startLine - 1 < allLines.size()) {
      output += allLines[region.startLine - 1] + "\n";
    }

    // Optimize and output the region
    auto opt = optimizeRegion(region.code, config, enableRecombine, collectCost,
                              verbose);
    output += opt.code;

    if (collectCost) {
      totalBefore.flops += opt.costBefore.flops;
      totalBefore.totalNodes += opt.costBefore.totalNodes;
      totalBefore.stmts += opt.costBefore.stmts;
      totalBefore.vars += opt.costBefore.vars;
      totalAfter.flops += opt.costAfter.flops;
      totalAfter.totalNodes += opt.costAfter.totalNodes;
      totalAfter.stmts += opt.costAfter.stmts;
      totalAfter.vars += opt.costAfter.vars;
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

  // Cost analysis output
  if (collectCost) {
    if (outputJson) {
      std::cout << "{\n";
      std::cout << "  \"before\": {\"flops\":" << totalBefore.flops
                << ",\"nodes\":" << totalBefore.totalNodes
                << ",\"stmts\":" << totalBefore.stmts
                << ",\"vars\":" << totalBefore.vars << "},\n";
      std::cout << "  \"after\": {\"flops\":" << totalAfter.flops
                << ",\"nodes\":" << totalAfter.totalNodes
                << ",\"stmts\":" << totalAfter.stmts
                << ",\"vars\":" << totalAfter.vars << "},\n";
      int saved = totalBefore.savedFlops(totalAfter);
      double pct = totalBefore.savedPercent(totalAfter);
      std::cout << "  \"saved\": {\"flops\":" << saved
                << ",\"percent\":" << std::round(pct * 10.0) / 10.0 << "}\n";
      std::cout << "}\n";
    } else {
      std::cout << "\n=== FLOP Cost Analysis ===\n";
      auto printCost = [](const char* label, const cse::CostResult& b,
                          const cse::CostResult& a) {
        std::cout << label << ":\n"
                  << "  Before:  " << b.flops << " flops, "
                  << b.totalNodes << " nodes, "
                  << b.stmts << " stmts, "
                  << b.vars << " vars\n"
                  << "  After:   " << a.flops << " flops, "
                  << a.totalNodes << " nodes, "
                  << a.stmts << " stmts, "
                  << a.vars << " vars\n";
        int saved = b.savedFlops(a);
        double pct = b.savedPercent(a);
        std::cout << "  Saved:   " << saved << " flops ("
                  << std::round(pct * 10.0) / 10.0 << "%)\n";
      };
      printCost("Total", totalBefore, totalAfter);
    }
  }

  return 0;
}
