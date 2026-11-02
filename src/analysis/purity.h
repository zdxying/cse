#pragma once
#include "ir/dag_node.h"

namespace cse {

// ===== The purity questions =====
//
// A pass that rewrites, duplicates, reorders or discards an expression is
// asking one of a few *distinct* questions about it. They are not
// interchangeable, and the answers were once spread over five files as seven
// differently-named helpers -- two of them both called `isTrivial` but with
// different meanings. Conflating them produced silent miscompiles: a rule that
// duplicated `2.0 * (f() + 1.0)` because it read the shallow per-node flag, and
// a user member method named `sin` treated as a builtin. This header is the
// single owner of the questions, each named by what it asks.
//
// The leaf fact is `DAGNode::pure` (dag_node.h): a call is pure when its callee
// is, a load when it reads a read-only location. It is deliberately NOT
// propagated to the parent, which is why the questions below exist.

// Does discarding `e`'s value lose an observable effect? An impure call writes
// or observes mutable state; `++`/`--` writes its operand; and an assignment
// spelled as an expression (the `a[i] = v` shape the builder did not lower into
// an AssignIR) writes its target. Arithmetic has no effect, so discarding its
// result is free -- but the increment sitting in the same expression is not.
inline bool hasObservableEffect(const DAGNode* e) {
  if (!e) return false;
  if (e->kind == NodeKind::Call && !e->pure) return true;
  if (isIncDec(e)) return true;
  if (e->kind == NodeKind::BinaryOp && e->op == OpCode::Assign) return true;
  for (const auto* op : e->operands)
    if (hasObservableEffect(op)) return true;
  return false;
}

// Is `e` referentially transparent -- safe to evaluate twice, share, or move?
//
// This is the *deep* question. `pure` is a per-node flag, not propagated to the
// parent: `a[i] * b` is a BinaryOp with `pure == true` even though `a[i]` is a
// non-shareable load, and `f() + 1.0` is a pure BinaryOp over an impure call. A
// rule that repeats or relocates a subtree must reach every descendant, which is
// what this walk does. Ask this -- never the bare `e->pure` -- whenever the
// answer justifies a rewrite.
inline bool isReferentiallyTransparent(const DAGNode* e) {
  if (!e) return true;
  if (!e->pure) return false;
  for (const auto* op : e->operands)
    if (!isReferentiallyTransparent(op)) return false;
  return true;
}

// Is `e` a side-effect-free lvalue access: a variable or constant, or an
// array/member/arrow access whose operands are themselves effect-free? Used to
// guard the removal of a complex-lvalue self-assignment (`a[i] = a[i]`).
inline bool isEffectFreeLvalue(const DAGNode* e) {
  if (!e) return false;
  switch (e->kind) {
    case NodeKind::Variable:
    case NodeKind::Constant:
      return true;
    case NodeKind::ArrayAccess:
    case NodeKind::MemberAccess:
    case NodeKind::ArrowAccess:
      for (const auto* op : e->operands)
        if (!isEffectFreeLvalue(op)) return false;
      return true;
    default:
      return false;
  }
}

// Is `e` a leaf value -- a constant or a variable? The operands that are cheap
// to reorder or compare structurally (used by algebraic commutativity sorting).
inline bool isLeafValue(const DAGNode* e) {
  return e && (e->kind == NodeKind::Constant || e->kind == NodeKind::Variable);
}

// Is `e` cheap enough to duplicate at a use site (value propagation)? A leaf,
// or a load marked shareable off a variable root. A load through a
// pointer/reference that may alias a later store is not shareable, so copying
// it to the use site would move the read past that store.
inline bool isCheapShareableValue(const DAGNode* e) {
  if (!e) return false;
  switch (e->kind) {
    case NodeKind::Constant:
    case NodeKind::Variable:
      return true;
    case NodeKind::MemberAccess:
    case NodeKind::ArrowAccess:
      return e->pure && e->operands.size() == 1 &&
             e->operands[0]->kind == NodeKind::Variable;
    default:
      return false;
  }
}

// Is it safe to evaluate `e` at a point where the original program would *not*
// have evaluated it?
//
// A hoisting rewrite moves a subexpression to the top of the statement list.
// That is fine for an expression that is merely conditionally *skipped*
// (`k ? a*b : 0` -- computing `a*b` unconditionally yields the same value), but
// not for one whose evaluation can trap: `k ? 1/k : 0` traps at k = 0 when the
// division becomes unconditional, and `k ? arr[1000] : 0` may fault.
//
// This is a *different* question from `isReferentiallyTransparent`, and the two
// disagree exactly on traps: `1/k` is referentially transparent (same value at
// the same point, safe to repeat or share) yet not speculatable. A pass that
// hoists must ask this one; a pass that only shares at a fixed point asks the
// other.
//
// The lazy operators are handled by their own semantics: hoisting a whole
// `cond ? a : b` (or `a && b`) keeps its internal short-circuit, so only the
// always-evaluated part -- the condition / left operand -- has to be
// speculatable. Hoisting the *branch* on its own is what must be refused.
//
// Conservative by construction:
//   * a non-pure node (impure call, non-shareable load, `++`/`--`) carries an
//     effect or observes memory, so it may not run unconditionally;
//   * a memory access may fault when its index is out of bounds, which the IR
//     cannot prove either way;
//   * a division or modulo whose divisor is not a non-zero constant may trap.
//     The IR carries no full type, so integer and floating division cannot be
//     told apart -- both are refused.
inline bool isSpeculatable(const DAGNode* e) {
  if (!e) return true;
  if (!e->pure) return false;
  switch (e->kind) {
    case NodeKind::ArrayAccess:
    case NodeKind::MemberAccess:
    case NodeKind::ArrowAccess:
      return false;
    case NodeKind::Ternary:
      // `c ? x : y` -- only the condition runs unconditionally.
      if (e->operands.size() != 3) return false;
      return isSpeculatable(e->operands[0]);
    case NodeKind::BinaryOp:
      if (e->op == OpCode::LogicalAnd || e->op == OpCode::LogicalOr) {
        // `a && b` / `a || b` -- only the left operand runs unconditionally.
        if (e->operands.size() != 2) return false;
        return isSpeculatable(e->operands[0]);
      }
      if (e->op == OpCode::Div || e->op == OpCode::Mod) {
        if (e->operands.size() != 2) return false;
        const DAGNode* divisor = e->operands[1];
        if (divisor->kind != NodeKind::Constant || divisor->constVal == 0)
          return false;
      }
      break;
    default:
      break;
  }
  for (const DAGNode* op : e->operands)
    if (!isSpeculatable(op)) return false;
  return true;
}

}  // namespace cse
