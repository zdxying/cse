#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "backend/codegen.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "passes/pass_manager.h"

static void printUsage(const char* prog) {
  std::cerr << "Usage: " << prog << " <input.cpp> [options]\n"
            << "Options:\n"
            << "  -r, --recombine    Enable expression recombination\n"
            << "  -h, --help         Show this help\n";
}

// Pipeline: source text → Lexer → Parser → IRBuilder → PassManager → CodeGen
// Each //@cse region is processed independently through this pipeline.
static std::string optimizeRegion(const std::string& code, bool enableRecombine) {
  // 1. Lex: tokenize source
  cse::Lexer lexer(code);
  auto tokens = lexer.tokenize();

  // 2. Parse: tokens → AST (FunctionDef, StructDef)
  cse::Parser parser(tokens);
  auto result = parser.parseAll();

  if (result.functions.empty() && result.structDefs.empty()) {
    return code;  // Nothing to optimize
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
      auto pm = cse::PassManager::createDefault(enableRecombine);
      pm.runAll(*methodMod);
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
  std::string optimized;
  bool emitStructs = true;
  for (auto& func : result.functions) {
    cse::IRModule module;
    cse::IRBuilder builder(&module);
    builder.buildFunction(*func);

    // Run passes
    auto pm = cse::PassManager::createDefault(enableRecombine);
    pm.runAll(module);

    // Generate code (emit struct defs only before first function)
    cse::CodeGen codegen;
    if (emitStructs) {
      optimized += codegen.generate(module, structPtrs, optStructs, func->templateParams);
      emitStructs = false;
    } else {
      optimized += codegen.generate(module, {}, {}, func->templateParams);
    }
  }

  // If no functions but have structs, emit structs only
  if (result.functions.empty() && !optStructs.empty()) {
    cse::IRModule emptyModule;
    cse::CodeGen codegen;
    optimized += codegen.generate(emptyModule, structPtrs, optStructs);
  }

  return optimized;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::string inputFile;
  bool enableRecombine = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "-r" || arg == "--recombine") {
      enableRecombine = true;
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
    output += optimizeRegion(region.code, enableRecombine);

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
  return 0;
}
