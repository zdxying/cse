#pragma once
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dag_node.h"
#include "ir_module.h"
#include "statement.h"
#include "stmt_walk.h"

namespace cse {

// Does the expression contain an impure (side-effecting) call?
inline bool hasImpureCall(DAGNode* e) {
  if (!e) return false;
  if (e->kind == NodeKind::Call && !e->pure) return true;
  for (auto* op : e->operands)
    if (hasImpureCall(op)) return true;
  return false;
}

// Does the expression do something that has to survive even when its value is
// thrown away? An impure call writes or observes mutable state; `++`/`--` writes
// its operand; and an assignment spelled as an expression (the `a[i] = v` shape
// the builder did not lower into an AssignIR) writes its target.
//
// This is `hasImpureCall` widened by the writes. Arithmetic has no effect, so
// discarding its result is free -- but the increment sitting in the same
// expression is not, and DCE asked only about calls. `unused = y++;` was
// therefore deleted whole and the increment of `y` disappeared with it.
inline bool hasSideEffect(const DAGNode* e) {
  if (!e) return false;
  if (e->kind == NodeKind::Call && !e->pure) return true;
  if (isIncDec(e)) return true;
  if (e->kind == NodeKind::BinaryOp && e->op == '=') return true;
  for (const auto* op : e->operands)
    if (hasSideEffect(op)) return true;
  return false;
}

// Add every variable occurrence in an expression to `counts`.
inline void countVarUses(DAGNode* e,
                         std::unordered_map<std::string, int>& counts) {
  if (!e) return;
  if (e->kind == NodeKind::Variable) counts[e->name]++;
  for (auto* op : e->operands) countVarUses(op, counts);
}

// Count how many times each variable name is used across a StmtIR tree.
//
// An element store counts its lvalue as a use of the root (and of its index
// operands), which is what keeps a declaration that is only ever stored into
// from looking unused.
inline std::unordered_map<std::string, int> countUses(StmtIR* root) {
  std::unordered_map<std::string, int> counts;
  forEachExprDeep(root, [&](DAGNode*& e) { countVarUses(e, counts); });
  return counts;
}

// Root variable of an lvalue expression: `a[i].m` -> "a"; no variable at all
// (e.g. a call result) -> "". Used to reason about stores through a computed
// lvalue.
inline std::string lvalueRoot(DAGNode* n) {
  while (n) {
    if (n->kind == NodeKind::Variable) return n->name;
    if (n->operands.empty()) return "";
    n = n->operands[0];
  }
  return "";
}

// Every variable name an expression writes: `++x` / `x--`, and an assignment
// spelled as an expression (`a[i] = v` arrives as `BinaryOp(=)` whenever the
// builder kept it unlowered). The whole subtree is walked, because a write can
// sit anywhere inside one.
inline void collectExprWrites(DAGNode* e,
                              std::unordered_set<std::string>& out) {
  if (!e) return;
  if (isIncDec(e) && !e->operands.empty()) {
    std::string root = lvalueRoot(e->operands[0]);
    if (!root.empty()) out.insert(root);
  } else if (e->kind == NodeKind::BinaryOp && e->op == '=' &&
             e->operands.size() == 2) {
    std::string root = lvalueRoot(e->operands[0]);
    if (!root.empty()) out.insert(root);
  }
  for (auto* op : e->operands) collectExprWrites(op, out);
}

// Collect every variable name a statement writes, nested statements included.
// Conditional writes count: callers use this to answer "may this have been
// written between A and B", and the conservative answer needs no dominator
// analysis.
//
// This is a *completeness* question, so it is answered by scanning the
// expression slots rather than by enumerating statement kinds. A write can sit
// in any slot, and the kind-by-kind version -- which listed the kinds it knew
// about and recursed into Block / ForLoop / IfElse -- never looked inside a
// declaration's initializer. In `double z = ++y;` the increment of `y` was
// therefore invisible, `y` counted as never written, and ValueProp inlined
// `double y = a;` at every use, including one that reads it *after* the
// increment:
//
//   double y = a; double z = ++y; return a * 100.0 + z;  // source: 100a + a + 1
//   -> double z = ++a; return z + 100 * a;               // emitted: 101a + 101
//
// `forEachExprDeep` (stmt_walk.h) owns the slot list, so this scan cannot go
// stale again when a statement kind or a slot is added.
//
// Two writes are not expression slots and are added explicitly:
//   * the lvalue root of a structured store (`a[i] = v;` / `p->f = v;` build an
//     AssignIR whose lvalue is not evaluated as an expression). Only the *root*
//     is recorded: some other pointer may alias the same object, so naming the
//     root is the conservative choice;
//   * a `for` update the builder normalized out of the expression tree into
//     "variable op= rhs" (`i = i + 1`), which is not visible as an expression
//     at all.
//
// A `VarDecl` is deliberately *not* recorded: it introduces a fresh
// (alpha-renamed) name rather than overwriting an existing value, and no
// consumer of this set wants declarations in it.
inline void collectWrittenNames(StmtIR* stmt,
                               std::unordered_set<std::string>& out) {
  if (!stmt) return;

  // The slots this statement owns, then its nested statements. Scanning the
  // slots with forEachExpr (stmt_walk.h) rather than by statement kind is what
  // makes the initializer case visible above; the recursion stays because the
  // two writes below are not expression slots at all.
  forEachExpr(stmt, [&](DAGNode*& e) { collectExprWrites(e, out); });

  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) collectWrittenNames(s.get(), out);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectWrittenNames(f->init.get(), out);
      collectWrittenNames(f->body.get(), out);
      if (f->updateOp != 0 && f->update &&
          f->update->kind == NodeKind::Variable) {
        out.insert(f->update->name);
      }
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectWrittenNames(ie->thenBranch.get(), out);
      collectWrittenNames(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (a->targetExpr) {
        std::string root = lvalueRoot(a->targetExpr);
        if (!root.empty()) out.insert(root);
      } else if (!a->target.empty()) {
        out.insert(a->target);
      }
      break;
    }
    default:
      break;
  }
}

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
      switch (node->op) {
        case '+': result = lhs->constVal + rhs->constVal; break;
        case '-': result = lhs->constVal - rhs->constVal; break;
        case '*': result = lhs->constVal * rhs->constVal; break;
        case '/':
          // Not folded when the divisor is zero. Integer division by zero is
          // undefined behaviour and floating-point division by zero is infinity
          // or NaN; zero is the one answer that is wrong for both, and the IR
          // carries no type that could tell the two apart. Leave the expression
          // standing -- the compiler can diagnose or evaluate it correctly.
          if (rhs->constVal == 0) return node;
          result = lhs->constVal / rhs->constVal;
          break;
        case 'e': result = (lhs->constVal == rhs->constVal) ? 1 : 0; break;
        case 'n': result = (lhs->constVal != rhs->constVal) ? 1 : 0; break;
        case '<': result = (lhs->constVal < rhs->constVal) ? 1 : 0; break;
        case '>': result = (lhs->constVal > rhs->constVal) ? 1 : 0; break;
        case 'l': result = (lhs->constVal <= rhs->constVal) ? 1 : 0; break;
        case 'g': result = (lhs->constVal >= rhs->constVal) ? 1 : 0; break;
        default: return node;
      }
      // Spelling is createConst()'s job: an empty text makes it re-derive the
      // literal from the value through formatConst(), which keeps `-0.0` from
      // collapsing to the integer `0` (i.e. +0.0). This local copy used to do
      // its own formatting and dropped the sign.
      return mod.createConst(result, "");
    }
  }
  return node;
}

}  // namespace cse
