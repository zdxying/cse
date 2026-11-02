// Class-level oracle for the write-set analysis (ir/location.{h,cpp}).
//
// Every pass that reasons about memory asks the same question: "which
// locations may this statement write?". The answer is produced by
// `collectWriteLocations` and then closed over reference edges by
// `AliasInfo::closeWrites`. Each pass used to assemble that answer itself (build
// + collect + close), and each place that forgot the closure, or listed the
// statement's expression slots by hand, silently missed a write. The recurring
// miscompiles of the 2026-10 audit (for-init reference not tracked, a
// declaration initializer not scanned, a store index mistaken for a write) are
// all one defect class: *a write the pass did not see*.
//
// This verifier pins the class through a shape table: it builds each shape from
// source, computes the whole-body closed write set, and asserts the roots that
// must be present (and the ones that must not). It is *curated*, not generative
// -- a new statement shape still needs a row here, so a human must think of it.
// What changes is the unit: one table covers the class, instead of one fixture
// per incident discovered in the field. A generator that enumerates every
// StmtIRKind and lvalue shape is the natural next step, but this is not it.
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_write_set.cpp bin/libcse.a
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "ir/location.h"

namespace {

std::unique_ptr<cse::IRModule> build(const std::string& src) {
  cse::CSEConfig cfg;
  cse::Lexer lexer(src, cfg);
  std::vector<cse::Token> tokens = lexer.tokenize();
  cse::Parser parser(tokens, cfg);
  cse::Parser::ParseResult parsed = parser.parseAll();
  auto mod = std::make_unique<cse::IRModule>();
  if (!parsed.functions.empty()) {
    cse::IRBuilder builder(mod.get(), cfg);
    builder.buildFunction(*parsed.functions.front());
  }
  return mod;
}

// The whole-body write set, exactly as a pass consumes it: collect, then close
// over reference-referent edges.
cse::LocationSet closedWrites(cse::IRModule& mod) {
  cse::AliasInfo aliases = cse::AliasInfo::build(mod.body.get());
  cse::LocationSet w;
  cse::collectWriteLocations(mod.body.get(), w);
  aliases.closeWrites(w);
  return w;
}

int failures = 0;

void expect(const char* what, bool ok, const std::string& detail) {
  if (!ok) ++failures;
  std::printf("%-58s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok && !detail.empty()) std::printf("      %s\n", detail.c_str());
}

std::string describe(const cse::LocationSet& w) {
  if (w.anything()) return "anything";
  std::string s;
  for (const auto& l : w.items()) {
    if (!s.empty()) s += ",";
    s += l.root.empty() ? "<none>" : l.root;
  }
  return s.empty() ? "<empty>" : s;
}

// One shape: build it, and assert the required / forbidden roots of the closed
// write set. `mustBeAnything` pins the wildcard an impure call contributes.
struct Shape {
  const char* name;
  const char* src;
  std::vector<const char*> mustHave;
  std::vector<const char*> mustNot;
  bool mustBeAnything = false;
};

void runShape(const Shape& s) {
  auto mod = build(s.src);
  cse::LocationSet w = closedWrites(*mod);
  const std::string d = "writes={" + describe(w) + "}";

  bool ok = true;
  if (s.mustBeAnything) {
    ok = w.anything();
  } else {
    if (w.anything()) ok = false;  // an unexpected wildcard is a failure too
    for (const char* r : s.mustHave)
      if (!w.containsRoot(r)) ok = false;
    for (const char* r : s.mustNot)
      if (w.containsRoot(r)) ok = false;
  }
  expect(s.name, ok, d);
}

}  // namespace

int main() {
  const Shape shapes[] = {
      // A write inside a declaration initializer: `++i` advances `i`. The
      // initializer is an expression slot the hand-listed walkers used to skip.
      {"decl-init write is seen (++i in an initializer)",
       "double f(double a){ int i = 0; double z = ++i; return z; }",
       {"i"}, {"z"}, false},

      // A reference declared in a for-init introduces an alias edge, so a write
      // through `r` must close to its referent `x`. Only a block-level
      // VarDecl used to be recorded, so `x` was missing (commit 62a400a).
      {"for-init reference closes to its referent",
       "double f(double x, int n){ double a = x;"
       " for (double& r = x; n > 0; --n) { r += 1.0; } return a + x; }",
       {"x", "r"}, {}, false},

      // A structured store writes the *root* of its lvalue; the index is read,
      // not written. Listing the store's slots by hand made the index look like
      // a write and the root easy to miss.
      {"element store writes the root, not the index",
       "double f(double* t, double a, int i){ t[i] = a; return t[i]; }",
       {"t"}, {"i"}, false},

      // A reference chain: `s -> r -> i`. The closure must run to a fixed point,
      // so a write through `s` also names `i`.
      {"reference chain closes to the base",
       "double f(double x){ double i = x; double& r = i; double& s = r;"
       " s = 2.0; return i; }",
       {"i", "r", "s"}, {}, false},

      // A nested block is a scope, not a barrier: its writes are part of the
      // statement's write set.
      {"nested block write is collected",
       "double f(double a){ double x = a; { x = 1.0; } return x; }",
       {"x"}, {}, false},

      // A `for` induction variable is written by the loop header (the update is
      // normalized out of the expression tree, so it needs its own case).
      {"for induction variable is written",
       "double f(double a, int n){ double s = 0.0;"
       " for (int k = 0; k < n; ++k) { s += a; } return s; }",
       {"k", "s"}, {}, false},

      // An unknown (hence impure) call may write anything: the set must carry
      // the wildcard, and `anything()` must short-circuit the roots.
      {"impure call contributes the wildcard",
       "double f(double x){ return h(x); }",
       {}, {}, true},

      // A conditional lvalue: the store is to s, never to the condition. The
      // root walk used to take operands[0] unconditionally, so the write set
      // named `c` and s stayed "unwritten" -- ValueProp then read s across the
      // store and returned the new value. An unnameable base is a wildcard.
      {"conditional lvalue is a wildcard, not a write to the condition",
       "struct S { double x; };"
       "double f(S s, int c){ (c ? s : s).x = 5.0; return s.x; }",
       {}, {}, true},

      // Same for an element store whose base is a conditional expression.
      {"conditional base element store is a wildcard",
       "double f(double* t, int c, int i, double a){"
       " (c ? t : t)[i] = a; return t[i]; }",
       {}, {}, true},

      // Negative control: a pure read writes nothing at all. A `return x + 1`
      // must not be mistaken for a write of `x`.
      {"a pure read writes nothing",
       "double f(double x){ return x + 1.0; }",
       {}, {"x"}, false},
  };

  for (const Shape& s : shapes) runShape(s);

  std::printf(failures == 0 ? "\nALL WRITE-SET CHECKS PASSED\n"
                            : "\n%d WRITE-SET CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
