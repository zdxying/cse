// Shape checks for CounterPropPass nested-scope handling.
//
// The pass folds `tensor[i]` to `tensor[<const>]` using the straight-line
// constant counters known at that point. The bug this pins: it used to descend
// into a nested scope (a `for` body, an `if` branch, a bare block) still
// carrying the *enclosing* counter map, so
//
//   int i = 0;
//   for (int k = 0; k < n; ++k) { t[i] = a; ++i; }
//
// folded the body's `t[i]` to `t[0]` -- even though `i` changes every
// iteration. The fix seeds each nested scope with the counters valid at its
// *entry* (minus the names the scope may write) and never lets a nested write
// escape back out.
//
// `bin/cse` cannot isolate this: CounterProp is only injected as the
// post-algebra pass by the FreeLB emitter, not by `createDefault`, so this
// verifier drives `CounterPropPass` directly (vectorLocalName = nullptr).
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_counter_prop.cpp bin/libcse.a
#include <cstdio>
#include <string>
#include <vector>

#include "backend/codegen.h"
#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "counter_prop.h"

namespace {

// Build one function's IR, run ONLY the CounterPropPass, and emit it.
std::string runCounterProp(const std::string& src) {
  cse::CSEConfig cfg;
  cse::Lexer lexer(src, cfg);
  std::vector<cse::Token> tokens = lexer.tokenize();
  cse::Parser parser(tokens, cfg);
  cse::Parser::ParseResult parsed = parser.parseAll();
  std::string out;
  for (auto& f : parsed.functions) {
    cse::IRModule mod;
    cse::IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);
    cse::CounterPropPass pass(nullptr);
    pass.run(mod);
    cse::CodeGen cg;
    out += cg.generate(mod, {}, {}, f->templateParams);
  }
  return out;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

int failures = 0;

void expect(const char* what, bool ok, const std::string& detail) {
  if (!ok) failures++;
  std::printf("%-52s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) std::printf("      generated:\n%s\n", detail.c_str());
}

// Count non-overlapping occurrences of `needle` in `hay`.
int count(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos;
       p = hay.find(needle, p + needle.size()))
    ++n;
  return n;
}

}  // namespace

int main() {
  // 1. The regression. A counter written inside the loop is not constant
  //    across iterations, so the body's index must stay symbolic. Before the
  //    fix this emitted `t[0] = a` (every iteration clobbering element 0).
  {
    const std::string src =
        "double f(double* t, double a, int n) {"
        " int i = 0;"
        " for (int k = 0; k < n; ++k) { t[i] = a; ++i; }"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("loop-written counter stays symbolic",
           has(out, "t[i]") && !has(out, "t[0]"), out);
  }

  // 2. A loop-invariant counter is still folded: the body does not write `i`,
  //    so it is constant at every iteration's entry.
  {
    const std::string src =
        "double f(double* t, double a, int n) {"
        " int i = 3;"
        " for (int k = 0; k < n; ++k) { t[i] = a; }"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("loop-invariant counter folds", has(out, "t[3]") && !has(out, "t[i]"),
           out);
  }

  // 3. Straight-line sequential folding still works: each `++i` advances the
  //    known value for the following store.
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 0;"
        " t[i] = a;"
        " ++i;"
        " t[i] = a;"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("straight-line folds t[0] then t[1]",
           has(out, "t[0]") && has(out, "t[1]") && !has(out, "t[i]"), out);
  }

  // 4. Copy-in: a bare nested block inherits the counters known at its entry.
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 2;"
        " { t[i] = a; }"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("nested block inherits entry counter", has(out, "t[2]") && !has(out, "t[i]"),
           out);
  }

  // 5. A constant `if` is folded and its taken branch is processed like
  //    straight-line code -- so the entry counter folds there too.
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 1;"
        " if (1) { t[i] = a; }"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("folded const-if inherits entry counter",
           has(out, "t[1]") && !has(out, "t[i]") && !has(out, "if"), out);
  }

  // 6. A write inside a nested block must not leak back out: the block's `i = 5`
  //    invalidates the enclosing knowledge, so the following `t[i]` stays
  //    symbolic. This is the mirror of case 4 -- the block gets the entry
  //    counter, but its result does not propagate.
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 1;"
        " { i = 5; }"
        " t[i] = a;"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("nested block write does not leak out", has(out, "t[i]") && !has(out, "t[1]"),
           out);
  }

  // 7. Same for an `if` branch: a non-constant condition means the branch may or
  //    may not run, so its write to the counter must invalidate it afterward.
  {
    const std::string src =
        "double f(double* t, double a, int c) {"
        " int i = 1;"
        " if (c) { i = 7; }"
        " t[i] = a;"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("if-branch write does not leak out", has(out, "t[i]") && !has(out, "t[1]"),
           out);
  }

  // 8. Each branch of a runtime `if` folds from the same entry state (case 4's
  //    copy-in, applied to both arms). A store that folded one arm's index from
  //    the other's write would show up as a mismatched count here.
  {
    const std::string src =
        "double f(double* t, double a, int c) {"
        " int i = 6;"
        " if (c) { t[i] = a; } else { t[i] = a; }"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("both branches fold from entry state",
           count(out, "t[6]") == 2 && !has(out, "t[i]"), out);
  }

  // 9. A write inside a declaration initializer must invalidate the counter it
  //    writes: `double z = ++i;` advances `i`, so the following store folds to
  //    t[1], not the stale t[0].
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 0;"
        " t[i] = a;"
        " double z = ++i;"
        " t[i] = a;"
        " return z;"
        "}";
    const std::string out = runCounterProp(src);
    // First store folds to t[0]; the second, after `++i`, must be t[1].
    expect("decl-init write advances the counter",
           count(out, "t[0]") == 1 && count(out, "t[1]") == 1, out);
  }

  // 10. A write through a reference must invalidate its referent: `r = 2`
  //     changes `i`, so the following store stays symbolic (it used to fold to
  //     the stale t[0]).
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 0;"
        " int& r = i;"
        " t[i] = a;"
        " r = 2;"
        " t[i] = a;"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    // First store folds to t[0]; the second must stay symbolic (not the stale
    // t[0]).
    expect("alias write invalidates the referent",
           count(out, "t[0]") == 1 && has(out, "t[i]"), out);
  }

  // 11. An increment through a reference advances the referent, so the following
  //     store must not fold to the stale t[0].
  {
    const std::string src =
        "double f(double* t, double a) {"
        " int i = 0;"
        " int& r = i;"
        " t[i] = a;"
        " ++r;"
        " t[i] = a;"
        " return 0;"
        "}";
    const std::string out = runCounterProp(src);
    expect("reference increment invalidates the referent",
           count(out, "t[0]") == 1 && has(out, "t[i]"), out);
  }

  std::printf(failures == 0 ? "\nALL COUNTER-PROP CHECKS PASSED\n"
                            : "\n%d COUNTER-PROP CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
