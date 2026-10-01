#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Directed Acyclic Graph node — the core of CSE.
// Each expression is a DAGNode; structurally identical subexpressions
// share the same node (deduplication at construction time via hash).
//
// ===== The interning protocol =====
//
// A node is *interned* when it is placed into IRModule's hash map, so a later
// request for the same node returns that one instead of a new node. Two rules
// make interning sound, and IRModule enforces both:
//
//  1. Identity is fixed before the node is interned. Factories write every
//     semantic field, then call recomputeHash(), and *nothing* writes a
//     semantic field afterwards. Touching one afterwards leaves `hash`
//     describing a node that no longer exists -- which is how `-x` and `--x`
//     once ended up as the same node, and how `(int)x` and `(float)x` did.
//
//  2. Identity covers every field that changes what the node denotes or does:
//     kind, op, name, pure, postfix, the operand list, and (for a constant) the
//     value. `sameContentAs()` is the single definition of that list and
//     `computeHash()` folds exactly the same list, so the two cannot disagree
//     about what makes two nodes interchangeable. IRModule::verify() checks the
//     two against each other at run time.
//
// `numText` and `symbol` are deliberately *not* part of the identity: they are
// two spellings of the same constant, and codegen renders any constant node as
// "the value constVal, as `symbol` when one was declared". Keeping them out is
// what lets createConst() dedupe by value and createSymbolicConst() attach a
// declared accessor to a value node that already exists.

namespace cse {

enum class NodeKind {
  Constant,
  Variable,
  BinaryOp,
  UnaryOp,
  ArrayAccess,   // base[index]
  MemberAccess,  // base.member
  ArrowAccess,   // base->member
  Call,
  Ternary,
  Cast,
};

// Forward declaration
class DAGNode;

struct NodeEqual {
  bool operator()(const DAGNode* a, const DAGNode* b) const;
};

struct DAGNode {
  DAGNode(NodeKind k, uint32_t id);

  NodeKind kind;
  uint32_t id;    // unique node id
  uint64_t hash;  // structural hash — drives CSE deduplication

  // Constant
  double constVal = 0;
  std::string numText;
  // Optional symbolic form of a constant (a declared accessor kept verbatim,
  // e.g. `ns::w<T>(1)`). Used for code emission only; the numeric constVal
  // still drives folding, dedup and CSE.
  std::string symbol;

  // Variable name; the member name of a MemberAccess/ArrowAccess; the cast type
  // of a Cast; or the operator spelling ("++" / "--") of an increment /
  // decrement node. See isIncDec().
  std::string name;

  // BinaryOp / UnaryOp
  char op = 0;

  // Operands (children in DAG) — for BinaryOp: [lhs, rhs]; for MemberAccess: [base]
  std::vector<DAGNode*> operands;

  // For Call / memory-load nodes: the value is referentially transparent
  // (pure call, or load from a read-only location). Non-pure nodes are never
  // deduplicated and must not be reordered across effects.
  bool pure = true;

  // Vector lane count for value-typed nodes (0/1 = scalar). Populated by the
  // IRBuilder for variables/parameters from their declared type; used by the
  // cost model to weight vector arithmetic. Deliberately NOT part of the
  // structural hash (it is type metadata, not expression structure).
  int vecDim = 0;

  // Only meaningful for an increment/decrement node: true for `x++` / `x--`,
  // false for `++x` / `--x`. The two forms yield different values, so this is
  // part of the node's identity.
  bool postfix = false;

  // Does `other` denote exactly the same node as this one? The one definition of
  // node identity; recomputeHash() and IRModule::verify() are written against
  // it.
  bool sameContentAs(const DAGNode& other) const;

  // Fold sameContentAs()'s field list into a hash. Pure function of the subtree,
  // so a const invariant check can recompute it.
  uint64_t computeHash() const;

  // Compute the hash from the (final) semantic fields. Call once, right before
  // the node is interned; never again afterwards.
  void recomputeHash() { hash = computeHash(); }
};

// Is `n` an increment/decrement node? `++x`, `x++`, `--x` and `x--` all arrive
// as a UnaryOp whose `name` is the operator spelling. One place owns that
// encoding; ask this rather than repeating the string comparison.
inline bool isIncDec(const DAGNode* n) {
  return n && n->kind == NodeKind::UnaryOp &&
         (n->name == "++" || n->name == "--");
}

}  // namespace cse
