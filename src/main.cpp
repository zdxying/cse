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
#include "../plugins/freelb/config.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "passes/pass_manager.h"

static void printUsage(const char* prog) {
  std::cerr << "Usage: " << prog << " <input.cpp> [options]\n"
            << "Options:\n"
            << "  -r, --recombine    Enable expression recombination\n"
            << "  -c, --cost         Analyze and report FLOP cost comparison\n"
            << "  --json             Output in JSON format (use with -c)\n"
            << "  -h, --help         Show this help\n";
}

struct OptResult {
  std::string code;
  cse::CostResult costBefore;
  cse::CostResult costAfter;
};

// Pipeline: source text → Lexer → Parser → IRBuilder → PassManager → CodeGen
// Each //@cse region is processed independently through this pipeline.
static OptResult optimizeRegion(const std::string& code, bool enableRecombine,
                                bool collectCost) {
  // 1. Create config with FreeLB defaults (skip __xx__, simplify T{1})
  cse::CSEConfig config = cse::freelb::createFreeLBConfig();

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

  // Emit using declarations and namespaces as raw text (not optimized)
  OptResult optResult;
  for (auto& ud : result.usingDecls) {
    optResult.code += "using " + ud->aliasName + " = " + ud->underlyingType + ";\n";
  }
  for (auto& ns : result.namespaces) {
    optResult.code += "namespace " + ns->name + " {\n";
    // Emit using declarations inside namespace
    for (auto& ud : ns->usingDecls) {
      optResult.code += "using " + ud->aliasName + " = " + ud->underlyingType + ";\n";
    }
    optResult.code += "}\n";
  }

  // 3. Optimize struct methods (each method gets its own IR pipeline)
  std::vector<cse::OptimizedStruct> optStructs;
  for (auto& sd : result.structDefs) {
    cse::OptimizedStruct os;
    os.def = sd.get();
    for (auto& method : sd->methods) {
      auto methodMod = std::make_unique<cse::IRModule>();
      cse::IRBuilder builder(methodMod.get());
      builder.buildFunction(*method);
      if (collectCost) {
        auto before = cse::analyzeCost(*methodMod);
        optResult.costBefore.flops += before.flops;
        optResult.costBefore.totalNodes += before.totalNodes;
        optResult.costBefore.stmts += before.stmts;
        optResult.costBefore.vars += before.vars;
      }
      auto pm = cse::PassManager::createDefault(enableRecombine);
      pm.runAll(*methodMod);
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
  for (auto& sd : result.structDefs) {
    if (sd->methods.empty()) {
      structPtrs.push_back(sd.get());
    }
  }

  // 4. Build IR for each function: AST → DAG-based IR
  bool emitStructs = true;
  for (auto& func : result.functions) {
    cse::IRModule module;
    cse::IRBuilder builder(&module);
    builder.buildFunction(*func);

    // Collect cost before optimization
    if (collectCost) {
      auto before = cse::analyzeCost(module);
      optResult.costBefore.flops += before.flops;
      optResult.costBefore.totalNodes += before.totalNodes;
      optResult.costBefore.stmts += before.stmts;
      optResult.costBefore.vars += before.vars;
    }

    // Run passes
    auto pm = cse::PassManager::createDefault(enableRecombine);
    pm.runAll(module);

    // Collect cost after optimization
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
      optResult.code += codegen.generate(module, structPtrs, optStructs, func->templateParams);
      emitStructs = false;
    } else {
      optResult.code += codegen.generate(module, {}, {}, func->templateParams);
    }
  }

  // If no functions but have structs, emit structs only
  if (result.functions.empty() && !optStructs.empty()) {
    cse::IRModule emptyModule;
    cse::CodeGen codegen;
    optResult.code += codegen.generate(emptyModule, structPtrs, optStructs);
  }

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

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "-r" || arg == "--recombine") {
      enableRecombine = true;
    } else if (arg == "-c" || arg == "--cost") {
      collectCost = true;
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
    auto opt = optimizeRegion(region.code, enableRecombine, collectCost);
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
