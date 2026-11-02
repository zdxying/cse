// Class-level oracle for the purity questions (analysis/purity.h).
//
// The pass bug that recurred most often was asking the *wrong* purity question:
// reading a node's shallow `pure` flag where the deep walk was needed (so
// `2.0 * (f() + 1.0)` duplicated the call), or treating a member method as a
// builtin. This verifier pins the predicates themselves -- the shallow/deep
// distinction in particular -- on nodes built through the real factories, so a
// future consolidation that collapses two questions into one fails here. It is
// a unit test, not a pass-level check: a pass that reaches for the shallow
// `e->pure` where the deep walk is needed is caught by the fixtures, not here.
//
// The load-bearing pair is the "shallow true / deep false" case: a Mul whose
// operand is a non-shareable load (or an impure call) carries `pure == true`
// because `pure` is a per-node flag, but is *not* referentially transparent.
// Every rewrite rule must ask the deep question.
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_purity.cpp bin/libcse.a
#include <cstdio>

#include "analysis/purity.h"
#include "ir/ir_module.h"

namespace {

int failures = 0;

void expect(const char* what, bool ok) {
  if (!ok) ++failures;
  std::printf("%-62s %s\n", what, ok ? "OK" : "FAIL");
}

}  // namespace

int main() {
  cse::IRModule mod;
  cse::DAGNode* a = mod.getVar("a");
  cse::DAGNode* i = mod.getVar("i");
  cse::DAGNode* two = mod.createConst(2.0);

  // A non-shareable load (a read that may be invalidated by a later store).
  cse::DAGNode* load = mod.createArrayAccess(a, i, /*shareable=*/false);
  expect("a non-shareable load is not referentially transparent",
         !cse::isReferentiallyTransparent(load));

  // The shallow/deep split. `pure` is per-node, so a Mul over the load is
  // shallow-pure but not deeply transparent -- the exact trap.
  cse::DAGNode* mul = mod.createBinaryOp(cse::OpCode::Mul, load, two);
  expect("a Mul over a non-shareable load still carries the shallow pure flag",
         mul->pure);
  expect("...but is not referentially transparent (deep)", 
         !cse::isReferentiallyTransparent(mul));

  // An impure call, and a sum over it (the `2.0 * (f() + 1.0)` shape).
  cse::DAGNode* callee = mod.getVar("f");
  cse::DAGNode* call = mod.createCall(callee, {a}, /*pure=*/false);
  expect("an impure call has an observable effect", cse::hasObservableEffect(call));
  expect("an impure call is not referentially transparent",
         !cse::isReferentiallyTransparent(call));

  cse::DAGNode* sum = mod.createBinaryOp(cse::OpCode::Add, call, two);
  expect("a sum over an impure call carries the shallow pure flag", sum->pure);
  expect("...but has an observable effect (deep)", cse::hasObservableEffect(sum));
  expect("...and is not referentially transparent (deep)",
         !cse::isReferentiallyTransparent(sum));

  // `++i`: an effect (it writes its operand) that a "discard" must not lose.
  cse::DAGNode* inc = mod.createPreIncDec(cse::OpCode::Inc, i);
  expect("++i has an observable effect", cse::hasObservableEffect(inc));
  expect("++i is not referentially transparent",
         !cse::isReferentiallyTransparent(inc));

  // An assignment spelled as an expression (`a[i] = a`) has an effect even
  // though the node is a BinaryOp with `pure == true`.
  cse::DAGNode* assign = mod.createBinaryOp(cse::OpCode::Assign, load, a);
  expect("an assignment expression has an observable effect",
         cse::hasObservableEffect(assign));

  // Plain arithmetic: transparent, no effect.
  cse::DAGNode* prod = mod.createBinaryOp(cse::OpCode::Mul, a, i);
  expect("pure arithmetic is referentially transparent",
         cse::isReferentiallyTransparent(prod));
  expect("pure arithmetic has no observable effect", !cse::hasObservableEffect(prod));

  // A shareable load (a read-only root) is transparent and effect-free.
  cse::DAGNode* roload = mod.createArrayAccess(a, i, /*shareable=*/true);
  expect("a shareable load is referentially transparent",
         cse::isReferentiallyTransparent(roload));
  expect("a shareable load has no observable effect",
         !cse::hasObservableEffect(roload));

  // isLeafValue: constants and variables only.
  expect("a constant is a leaf value", cse::isLeafValue(two));
  expect("a variable is a leaf value", cse::isLeafValue(a));
  expect("a load is not a leaf value", !cse::isLeafValue(load));

  // isCheapShareableValue: leaves, plus a shareable member/arrow load off a
  // variable root.
  expect("a variable is cheap and shareable", cse::isCheapShareableValue(a));
  expect("a non-shareable load is not cheap+shareable",
         !cse::isCheapShareableValue(load));
  expect("a shareable member load off a variable is cheap+shareable",
         cse::isCheapShareableValue(mod.createMemberAccess(a, "m", true)));
  expect("a Mul is not cheap+shareable", !cse::isCheapShareableValue(mul));

  // isEffectFreeLvalue: variables, constants and pure element/member accesses,
  // but not a call or an assignment.
  expect("a variable is an effect-free lvalue", cse::isEffectFreeLvalue(a));
  expect("a[i] is an effect-free lvalue", cse::isEffectFreeLvalue(load));
  expect("a call is not an effect-free lvalue", !cse::isEffectFreeLvalue(call));
  expect("an assignment expression is not an effect-free lvalue",
         !cse::isEffectFreeLvalue(assign));

  // isSpeculatable: the *third* question, and the one CSE was missing. It is not
  // "may this be repeated" (that is isReferentiallyTransparent) but "may this run
  // where the source did not run it". The two disagree exactly on traps:
  // `1/k` is transparent yet not speculatable.
  cse::DAGNode* k = mod.getVar("k");
  cse::DAGNode* one = mod.createConst(1.0);
  cse::DAGNode* div = mod.createBinaryOp(cse::OpCode::Div, one, k);
  expect("pure arithmetic is speculatable", cse::isSpeculatable(prod));
  expect("1/k is referentially transparent", cse::isReferentiallyTransparent(div));
  expect("...but 1/k is NOT speculatable (it traps at k = 0)",
         !cse::isSpeculatable(div));
  // A division by a non-zero constant cannot trap.
  expect("1/2.0 is speculatable",
         cse::isSpeculatable(mod.createBinaryOp(cse::OpCode::Div, one,
                                                mod.createConst(2.0))));
  // A memory access may fault, so it may not be speculated even when shareable.
  expect("a shareable load is NOT speculatable (it may fault)",
         !cse::isSpeculatable(roload));
  // Hoisting a whole conditional keeps its internal short-circuit, so only the
  // condition has to be speculatable -- the guarded arm is not evaluated here.
  cse::DAGNode* cond = mod.createBinaryOp(cse::OpCode::Gt, k, one);
  expect("a ternary over a trapping arm is speculatable (the arm stays guarded)",
         cse::isSpeculatable(mod.createTernary(cond, div, one)));
  // ...but a ternary whose *condition* traps is not.
  expect("a ternary whose condition traps is NOT speculatable",
         !cse::isSpeculatable(mod.createTernary(div, one, one)));
  // An impure call is never speculatable: it performs its effect wherever it runs.
  expect("an impure call is NOT speculatable", !cse::isSpeculatable(call));

  std::printf(failures == 0 ? "\nALL PURITY CHECKS PASSED\n"
                            : "\n%d PURITY CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
