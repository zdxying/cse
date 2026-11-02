#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "opcode.h"

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
//     value and its int/float kind. That list is written once, in the
//     CSE_DAGNODE_IDENTITY_FIELDS macro below, which both `sameContentAs()` and
//     `computeHash()` expand -- so the two cannot disagree about what makes two
//     nodes interchangeable. IRModule::verify() additionally checks the stored
//     hash against a fresh computeHash() at run time.
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

// The numeric kind of an expression, as far as the optimizer can prove it.
//
// The IR is not a typed IR: a node carries no full C++ type. But a handful of
// rewrites are only value-preserving when the expression keeps its *numeric
// kind* -- replacing a floating expression with an integer constant turns
// x/x followed by t/2 into 1/2 (integer division), and dropping the only
// floating factor of i * 2.0 turns a double multiply into an integer add.
// Those defects are all "kind erased", so the kind has to be recorded.
//
// It is *derived* metadata, not identity: two nodes that denote the same value
// are the same node regardless of how precisely we know their type. It is
// therefore NOT part of CSE_DAGNODE_IDENTITY_FIELDS, exactly like vecDim.
//
//   Unknown -- cannot prove either way (template parameter, member, call).
//              A rewrite that would need the kind must not fire.
//   Int     -- integral (int/unsigned/long/size_t/bool, or a cast to one).
//   Float   -- floating point (float/double, or a cast to one).
enum class NumericKind { Unknown, Int, Float };

// The kind of a binary operation's result: floating when either side is,
// integral when both are, and unknown as soon as one side is unknown.
inline NumericKind joinNumericKind(NumericKind a, NumericKind b) {
  if (a == NumericKind::Unknown || b == NumericKind::Unknown)
    return NumericKind::Unknown;
  if (a == NumericKind::Float || b == NumericKind::Float)
    return NumericKind::Float;
  return NumericKind::Int;
}

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
  // Does the constant denote a floating-point literal rather than an integer?
  // This is part of the identity: `2.0` and `2` have the same value but denote
  // different C++ expressions (`2.0 / 3` is 0.666..., `2 / 3` is 0), so they
  // must not be deduplicated into one node. It is set from the source spelling
  // for a literal, and propagated by the constant folder for a folded one.
  bool isFloat = false;
  std::string numText;
  // Optional symbolic form of a constant (a declared accessor kept verbatim,
  // e.g. `ns::w<T>(1)`). Used for code emission only; the numeric constVal
  // still drives folding, dedup and CSE.
  std::string symbol;

  // Variable name; the member name of a MemberAccess/ArrowAccess; the cast type
  // of a Cast; or the operator spelling ("++" / "--") of an increment /
  // decrement node. See isIncDec().
  std::string name;

  // The operator of a BinaryOp / UnaryOp / Ternary (OpCode::None otherwise).
  // The classification is a property of this value, not of a character; see
  // opcode.h for why. The spelling used by code emission is opSpelling(op).
  OpCode op = OpCode::None;

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
  // Vector lane count for value-typed nodes (0/1 = scalar). Populated by the
  // IRBuilder for variables/parameters from their declared type; used by the
  // cost model to weight vector arithmetic. Deliberately NOT part of the
  // structural hash (it is type metadata, not expression structure).
  int vecDim = 0;

  // Numeric kind of the value this node denotes (see NumericKind above).
  // Populated by the factories: a constant from isFloat, a variable from its
  // declaration, a binary/unary node from its operands, a cast from its type.
  // Like vecDim it is derived metadata and NOT part of the identity.
  NumericKind numKind = NumericKind::Unknown;

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

// The identity field table: every field that decides whether two nodes denote
// the same node, listed exactly once. DAGNode::sameContentAs() and
// DAGNode::computeHash() (ir/ir_module.cpp) both expand this macro, so the
// compared set and the hashed set are the same set *by construction* -- a field
// added here is both compared and hashed, and neither function can be updated
// without the other.
//
// Each field's own rule lives in its `identityEqual_<field>` /
// `identityHash_<field>` helper:
//   * `operands` compares element identity (node ids) and the count;
//   * `constant` compares a constant's raw bits (so `-0.0` and `+0.0` stay
//     distinct) and its int/float kind, and is vacuous for a non-constant node.
#define CSE_DAGNODE_IDENTITY_FIELDS(X) \
  X(kind)                              \
  X(op)                                \
  X(name)                              \
  X(pure)                              \
  X(postfix)                           \
  X(operands)                          \
  X(constant)

// Is `n` an increment/decrement node? `++x`, `x++`, `--x` and `x--` all arrive
// as a UnaryOp with OpCode::Inc / OpCode::Dec. One place owns that encoding; ask
// this rather than repeating the test.
inline bool isIncDec(const DAGNode* n) {
  return n && n->kind == NodeKind::UnaryOp && isIncDecOp(n->op);
}

}  // namespace cse
