#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_module.h"
#include "ir/ir_builder.h"
#include "passes/pass_manager.h"
#include "backend/codegen.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.cpp> [options]\n"
              << "Options:\n"
              << "  -r, --recombine    Enable expression recombination\n"
              << "  -h, --help         Show this help\n";
}

// Apply CSE optimization to a region
static std::string optimizeRegion(const std::string& code, bool enableRecombine) {
    // Lex
    cse::Lexer lexer(code);
    auto tokens = lexer.tokenize();

    // Parse
    cse::Parser parser(tokens);
    auto result = parser.parseAll();

    if (result.functions.empty()) {
        return code;  // Nothing to optimize
    }

    // Build IR for each function
    std::string optimized;
    for (auto& func : result.functions) {
        cse::IRModule module;
        cse::IRBuilder builder(&module);
        builder.buildFunction(*func);

        // Run passes
        auto pm = cse::PassManager::createDefault(enableRecombine);
        pm.runAll(module);

        // Generate code
        cse::CodeGen codegen;
        optimized += codegen.generate(module);
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
    std::string source((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
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
