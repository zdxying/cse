// Effect-preservation oracle.
//
// Every optimization must keep the program's observable effects: an impure call
// or an increment/decrement that was there before a pass must still be there
// after it, the same number of times. This is the invariant behind three
// separate wrong-code bugs:
//
//   * `2 * f() -> f() + f()`  duplicates an impure call,
//   * DCE deleting `unused = y++;`  drops the increment,
//   * an algebraic identity discarding an effectful operand.
//
// The oracle counts the effects (analysis/effects.h) before and after each pass
// and reports a drop -- or, for a pass that does not declare mayDuplicateEffects,
// an addition. Stores are excluded: dead-store elimination legitimately removes
// a store to private storage.
//
// Two layers: unit checks on the comparison itself (so the oracle is known to
// fire), then a sweep over every fixture region with the check enabled on a real
// pipeline.
//
// Usage: verify_effect_preservation <fixture.cpp> [more fixtures...]
//   Exit 0 iff every region preserves its effects.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/effects.h"
#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "passes/pass_manager.h"
#include "sweep.h"

using namespace cse;

namespace {

int failures = 0;

void expect(const char* what, bool ok) {
  std::printf("%-56s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

// --- layer 1: the comparison itself must fire on a drop / a duplication ------

void unitChecks() {
  EffectSignature a{{"call:f", 1}, {"incdec:++:pre:x", 1}};
  EffectSignature same{{"call:f", 1}, {"incdec:++:pre:x", 1}};
  EffectSignature dropped{{"call:f", 1}};
  EffectSignature duplicated{{"call:f", 2}, {"incdec:++:pre:x", 1}};

  expect("identical signatures pass",
         effectViolation(a, same, /*mayDuplicate=*/false).empty());
  expect("a dropped effect is a violation",
         !effectViolation(a, dropped, false).empty());
  expect("a duplicated effect is a violation",
         !effectViolation(a, duplicated, false).empty());
  expect("a duplicated effect is allowed when declared",
         effectViolation(a, duplicated, /*mayDuplicate=*/true).empty());
  expect("a dropped effect is a violation even when duplication is allowed",
         !effectViolation(a, dropped, /*mayDuplicate=*/true).empty());
}

// --- layer 2: the counter sees effects in real IR ---------------------------

// Parse one function and return the effect signature of its body.
bool signatureOf(const std::string& src, EffectSignature& out) {
  CSEConfig cfg;
  try {
    Lexer lexer(src, cfg);
    auto tokens = lexer.tokenize();
    Parser parser(tokens, cfg);
    auto res = parser.parseAll();
    if (res.functions.empty()) return false;
    IRModule mod;
    IRBuilder builder(&mod, cfg);
    builder.buildFunction(*res.functions.front());
    out = effectSignature(mod.body.get());
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void counterChecks() {
  EffectSignature sig;
  // Two calls to the same impure function count twice.
  if (signatureOf("double f(double x) { return g(x) + g(x); }", sig)) {
    expect("two impure calls are counted twice",
           sig.count("call:g") == 1 && sig.at("call:g") == 2);
  } else {
    expect("two impure calls are counted twice", false);
  }
  // An increment is an effect, and prefix/postfix are distinguished.
  if (signatureOf("double f(double y) { return y++; }", sig)) {
    expect("a postfix increment is counted",
           sig.count("incdec:++:post") == 1 && sig.at("incdec:++:post") == 1);
  } else {
    expect("a postfix increment is counted", false);
  }
  // A pure arithmetic expression has no effects.
  if (signatureOf("double f(double a, double b) { return a * b + a * b; }", sig)) {
    expect("pure arithmetic has no effects", sig.empty());
  } else {
    expect("pure arithmetic has no effects", false);
  }
  // The loop's own counter step is loop-control: unrolling removes the loop and
  // its counter, so `++i` must NOT be counted -- otherwise every legitimate
  // unroll would report a dropped effect.
  if (signatureOf("double f(double a) { double s = 0.0; for (int i = 0; i < 3; ++i) { s += a; } return s; }", sig)) {
    expect("the loop counter step is not counted",
           sig.count("incdec:++:pre") == 0);
  } else {
    expect("the loop counter step is not counted", false);
  }
  // An increment of anything *other* than the loop's own counter is a real
  // effect: such a loop is not a counted loop (LoopUnroll only unrolls an
  // update that advances the induction variable declared in the init), so it is
  // never unrolled and the increment must survive every pass. Counting it is
  // what lets the oracle see a pass drop it; countStmtEffects used to skip *any*
  // inc/dec in the update slot, so this increment was invisible.
  if (signatureOf("double f(double g, double n) { for (int i = 0; i < n; ++g) {} return g; }", sig)) {
    expect("a non-counter for-update increment is counted",
           sig.count("incdec:++:pre") == 1 && sig.at("incdec:++:pre") == 1);
  } else {
    expect("a non-counter for-update increment is counted", false);
  }
}

// --- layer 3: every fixture region preserves its effects --------------------

Result checkRegion(const std::string& src, std::string& msg) {
  CSEConfig cfg;
  cfg.assumeNumericCommutative = true;
  cfg.assumeNumericAssociative = true;
  cfg.allowFpReassoc = true;

  std::vector<Token> tokens;
  Parser::ParseResult res;
  try {
    Lexer lexer(src, cfg);
    tokens = lexer.tokenize();
    Parser parser(tokens, cfg);
    res = parser.parseAll();
  } catch (const std::exception& e) {
    msg = std::string("frontend rejected the region: ") + e.what();
    return Result::Skip;
  }
  if (res.functions.empty()) {
    msg = "no function to check";
    return Result::Skip;
  }

  for (const auto& f : res.functions) {
    IRModule mod;
    IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);

    auto pm = PassManager::createDefault(cfg, false, nullptr, nullptr);
    pm.setCheckEffects(true);
    pm.runAll(mod, false);

    if (!pm.effectViolations().empty()) {
      msg = "function '" + f->name + "':\n";
      for (const auto& v : pm.effectViolations()) msg += v;
      return Result::Fail;
    }
  }
  return Result::Pass;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture.cpp> [more...]\n", argv[0]);
    return 2;
  }

  unitChecks();
  counterChecks();

  const int rc = sweepRegions(argc, argv, "effect-preservation", "checks",
                              /*countAllRegions=*/false, /*printSkips=*/false,
                              checkRegion);
  return (rc == 0 && failures == 0) ? 0 : 1;
}
