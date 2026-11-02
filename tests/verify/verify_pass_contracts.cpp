// Pass-contract oracle.
//
// Two contracts every pass in the default pipeline must honour:
//
//  1. A pass that is wired into the pipeline and enabled must actually be able
//     to fire. A pass that never changes any fixture across the whole suite is
//     either dead code or gated shut -- exactly the failure mode where `-s -r`
//     silently dropped recombination because `-s` cleared the licence the pass
//     needed. This runs each pass in isolation (using the D4 fingerprint to see
//     whether it changed anything) and requires every pass to change at least
//     one function.
//
//  2. A pass declares whether it preserves the observable effects (Pass::
//     preservesEffects). Only loop unrolling may say no. This pins the set so a
//     new pass that quietly stops preserving effects cannot slip in unnoticed.
//
// Usage: verify_pass_contracts <fixture.cpp> [more fixtures...]
//   Exit 0 iff every pass fires somewhere and the effect contracts are as
//   expected.

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "ir/ir_printer.h"
#include "passes/pass_manager.h"
#include "sweep.h"

using namespace cse;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture.cpp> [more...]\n", argv[0]);
    return 2;
  }

  const CSEConfig cfg = aggressive();

  std::map<std::string, int> runs;      // pass -> functions it ran on
  std::map<std::string, int> changed;   // pass -> functions it changed
  std::set<std::string> nonPreserving;  // passes declaring preservesEffects()==false
  std::set<std::string> crossStmt;      // passes declaring crossesStatements()==true

  for (int ai = 1; ai < argc; ++ai) {
    bool ok = false;
    std::string source = readFile(argv[ai], ok);
    if (!ok) {
      std::fprintf(stderr, "cannot read %s\n", argv[ai]);
      return 2;
    }
    for (const auto& r : RegionExtractor().extract(source)) {
      // A directive region is passed through by the driver, so no pass ever
      // sees it; counting it here would be counting a pipeline that never runs.
      if (r.hasDirectives) continue;
      std::vector<Token> tokens;
      Parser::ParseResult res;
      try {
        Lexer lexer(r.code, cfg);
        tokens = lexer.tokenize();
        Parser parser(tokens, cfg);
        res = parser.parseAll();
      } catch (const std::exception&) {
        continue;  // unparseable region: not this oracle's concern
      }
      for (const auto& f : res.functions) {
        IRModule mod;
        IRBuilder builder(&mod, cfg);
        builder.buildFunction(*f);

        auto pm = PassManager::createDefault(cfg, false, nullptr, nullptr);
        for (const auto& pass : pm.passes()) {
          const std::string before = dumpModule(mod);
          pass->run(mod);
          runs[pass->name()]++;
          if (dumpModule(mod) != before) changed[pass->name()]++;
          if (!pass->preservesEffects()) nonPreserving.insert(pass->name());
          if (pass->crossesStatements()) crossStmt.insert(pass->name());
        }
      }
    }
  }

  int failures = 0;

  // 1. every pass fires somewhere.
  std::printf("pass activity over the fixture suite:\n");
  for (const auto& kv : runs) {
    const int n = changed.count(kv.first) ? changed.at(kv.first) : 0;
    std::printf("  %-20s %d/%d functions changed\n", kv.first.c_str(), n,
                kv.second);
    if (n == 0) {
      std::printf("FAIL  pass '%s' never changed anything (dead or gated shut)\n",
                  kv.first.c_str());
      ++failures;
    }
  }
  if (runs.empty()) {
    std::printf("FAIL  no pass ran: the fixture sweep saw no functions\n");
    ++failures;
  }

  // 2. only LoopUnroll may forgo effect preservation.
  const std::set<std::string> expected = {"LoopUnroll"};
  if (nonPreserving != expected) {
    std::printf("FAIL  effect-preservation contract changed: non-preserving = {");
    for (const auto& n : nonPreserving) std::printf("%s ", n.c_str());
    std::printf("}, expected {LoopUnroll}\n");
    ++failures;
  } else {
    std::printf("ok    only LoopUnroll forgoes effect preservation\n");
  }

  // 3. exactly these passes reuse across statements. Each of them builds an
  // AliasInfo at entry (grep AliasInfo::build in src/passes/); a pass that
  // declares crossesStatements() without consulting the write set -- or a pass
  // that silently starts reusing across statements without declaring -- is a
  // miscompile waiting for a fixture, so the contract is pinned here.
  // counter-prop also builds an AliasInfo but is not in the default pipeline
  // (FreeLB injects it through the PostAlgebra slot), so it never appears in
  // `pm.passes()` here; its declaration is pinned by verify_counter_prop.
  const std::set<std::string> crossExpected = {"CSE", "LoopUnroll", "Cleanup",
                                               "dce", "value-prop"};
  if (crossStmt != crossExpected) {
    std::printf("FAIL  cross-statement contract changed: declaring = {");
    for (const auto& n : crossStmt) std::printf("%s ", n.c_str());
    std::printf("}, expected {");
    for (const auto& n : crossExpected) std::printf("%s ", n.c_str());
    std::printf("}\n");
    ++failures;
  } else {
    std::printf("ok    cross-statement passes declare the write-set contract\n");
  }

  std::printf(failures == 0 ? "pass-contracts: OK\n" : "pass-contracts: FAILED\n");
  return failures == 0 ? 0 : 1;
}
