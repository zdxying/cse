#pragma once
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../analysis/purity.h"
#include "dag_node.h"
#include "ir_module.h"
#include "statement.h"
#include "stmt_walk.h"

namespace cse {

// The purity questions (`hasObservableEffect`, `isReferentiallyTransparent`,
// `isEffectFreeLvalue`, `isLeafValue`, `isCheapShareableValue`) live in
// analysis/purity.h -- the single owner, so a pass cannot reach for a
// differently-named copy. This header is only about rebuilding subtrees.

// Rebuild `node` with a new operand list, through IRModule's factories -- so the
// result is hashed and interned exactly like a node the frontend built.
//
// This is the single place that knows the node-kind -> factory mapping. It used
// to be copied into every pass that rewrote a subtree (substitute, ValueProp,
// the unroller), and the copies had drifted: ValueProp's dropped the ++/--
// spelling, and none of them handled Cast or Ternary -- so a substitution into a
// cast was silently discarded.
//
// Returns `node` unchanged when the kind has no rebuild rule or the arity does
// not match, so callers can treat "nothing to do" and "cannot rebuild" alike.
inline DAGNode* rebuildWithOperands(IRModule& mod, DAGNode* node,
                                    const std::vector<DAGNode*>& ops) {
  if (!node) return nullptr;
  switch (node->kind) {
    case NodeKind::BinaryOp:
      if (ops.size() == 2) return mod.createBinaryOp(node->op, ops[0], ops[1]);
      break;
    case NodeKind::UnaryOp:
      if (ops.size() == 1) {
        // The spelling and the prefix/postfix form are part of the node's
        // identity, so they go through the factory. Patching them onto the
        // returned node would leave its hash describing a different node.
        if (isIncDec(node)) {
          return node->postfix ? mod.createPostIncDec(node->op, ops[0])
                               : mod.createPreIncDec(node->op, ops[0]);
        }
        return mod.createUnaryOp(node->op, ops[0]);
      }
      break;
    case NodeKind::ArrayAccess:
      if (ops.size() == 2) return mod.createArrayAccess(ops[0], ops[1], node->pure);
      break;
    case NodeKind::MemberAccess:
      if (ops.size() == 1)
        return mod.createMemberAccess(ops[0], node->name, node->pure);
      break;
    case NodeKind::ArrowAccess:
      if (ops.size() == 1)
        return mod.createArrowAccess(ops[0], node->name, node->pure);
      break;
    case NodeKind::Call:
      if (!ops.empty()) {
        std::vector<DAGNode*> args(ops.begin() + 1, ops.end());
        return mod.createCall(ops[0], args, node->pure);
      }
      break;
    case NodeKind::Cast:
      if (ops.size() == 1) return mod.createCast(node->name, ops[0]);
      break;
    case NodeKind::Ternary:
      if (ops.size() == 3) return mod.createTernary(ops[0], ops[1], ops[2]);
      break;
    case NodeKind::Constant:
    case NodeKind::Variable:
      break;
  }
  return node;
}

// Replace all Variable nodes with the given name in a DAG subtree.
// Returns the (possibly new) root of the subtree.
inline DAGNode* substitute(IRModule& mod, DAGNode* root,
                           const std::string& name, DAGNode* replacement) {
  if (!root) return nullptr;
  if (root->kind == NodeKind::Variable && root->name == name) return replacement;
  if (root->operands.empty()) return root;

  bool changed = false;
  std::vector<DAGNode*> newOps;
  newOps.reserve(root->operands.size());
  for (auto* op : root->operands) {
    DAGNode* r = substitute(mod, op, name, replacement);
    newOps.push_back(r);
    if (r != op) changed = true;
  }
  if (!changed) return root;
  return rebuildWithOperands(mod, root, newOps);
}

// Constant folding: if a BinaryOp has two Constant operands, compute the result.
inline DAGNode* foldConst(IRModule& mod, DAGNode* node) {
  if (!node) return nullptr;
  if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
    DAGNode* lhs = foldConst(mod, node->operands[0]);
    DAGNode* rhs = foldConst(mod, node->operands[1]);
    if (lhs != node->operands[0] || rhs != node->operands[1]) {
      // Rebuild through the factory to keep the hash map consistent.
      node = mod.createBinaryOp(node->op, lhs, rhs);
    }
    lhs = node->operands[0];
    rhs = node->operands[1];
    // Do not fold declared/symbolic constants into numeric literals; the
    // declared symbol must survive to code emission.
    if (lhs->kind == NodeKind::Constant && lhs->symbol.empty() &&
        rhs->kind == NodeKind::Constant && rhs->symbol.empty()) {
      double result = 0;
      // The result of an arithmetic fold is floating when either operand is;
      // a comparison yields an integer (0/1).
      bool resultFloat = lhs->isFloat || rhs->isFloat;
      switch (node->op) {
        case OpCode::Add: result = lhs->constVal + rhs->constVal; break;
        case OpCode::Sub: result = lhs->constVal - rhs->constVal; break;
        case OpCode::Mul: result = lhs->constVal * rhs->constVal; break;
        case OpCode::Div: {
          // Not folded when the divisor is zero. Integer division by zero is
          // undefined behaviour and floating-point division by zero is infinity
          // or NaN; zero is the one answer that is wrong for both, so leave the
          // expression standing.
          if (rhs->constVal == 0) return node;
          double lv = lhs->constVal;
          double rv = rhs->constVal;
          double q = lv / rv;
          // When both operands are *integer* constants the source may be an
          // integer division, which truncates, so only fold when the quotient is
          // exact. If either operand is floating it is a floating division and
          // the double quotient is exact. The int/float kind now survives on the
          // node (see dag_node.h::isFloat), so this no longer has to guess from
          // the value.
          bool bothInt = !lhs->isFloat && !rhs->isFloat;
          if (bothInt && std::floor(q) != q) return node;
          result = q;
          break;
        }
        case OpCode::Eq: result = (lhs->constVal == rhs->constVal) ? 1 : 0; resultFloat = false; break;
        case OpCode::Ne: result = (lhs->constVal != rhs->constVal) ? 1 : 0; resultFloat = false; break;
        case OpCode::Lt: result = (lhs->constVal < rhs->constVal) ? 1 : 0; resultFloat = false; break;
        case OpCode::Gt: result = (lhs->constVal > rhs->constVal) ? 1 : 0; resultFloat = false; break;
        case OpCode::Le: result = (lhs->constVal <= rhs->constVal) ? 1 : 0; resultFloat = false; break;
        case OpCode::Ge: result = (lhs->constVal >= rhs->constVal) ? 1 : 0; resultFloat = false; break;
        default: return node;
      }
      // Spelling is createConstTyped()'s job: an empty text makes it re-derive
      // the literal from the value through formatConst(), which keeps `-0.0`
      // from collapsing to the integer `0` (i.e. +0.0) and keeps a floating
      // whole value's decimal point. This local copy used to do its own
      // formatting and dropped both.
      //
      // Non-finite results are *not* folded. There is no C++ literal spelling
      // for inf or NaN: formatConst() would emit "inf", and the generated file
      // then fails to compile ('inf' was not declared in this scope). Folding is
      // an optimization, so leaving `1e308 * 10.0` standing is correct and keeps
      // the output compilable. (`1e308 * 10.0` in the source still evaluates to
      // inf at run time, which is what the unoptimized program does too.)
      if (!std::isfinite(result)) return node;
      return mod.createConstTyped(result, resultFloat, "");
    }
  }
  return node;
}

}  // namespace cse
