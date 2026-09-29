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

// Collect every variable name a statement writes, nested statements included.
// Conditional writes count: callers use this to answer "may this have been
// written between A and B", and the conservative answer needs no dominator
// analysis.
//
// Three spellings of a write occur in this IR:
//   * `x = v;`                 -> AssignIR, plain `target`
//   * `a[i] = v;` / `p->f = v;`-> AssignIR, `targetExpr` (only the *root* is
//     recorded: some other pointer may alias the same object, so naming the
//     root is the conservative choice)
//   * `++x;` / `--x;`          -> a UnaryOp statement
// The second form also arrives as an ExprStmt wrapping an opaque
// `BinaryOp(=)` whenever the builder kept it unlowered, so both spellings are
// handled -- missing one of them is exactly how a store becomes invisible to
// the passes that ask this question.
//
// A `VarDecl` is deliberately *not* recorded: it introduces a fresh
// (alpha-renamed) name rather than overwriting an existing value, and no
// consumer of this set wants declarations in it.
inline void collectWrittenNames(StmtIR* stmt,
                               std::unordered_set<std::string>& out) {
  if (!stmt) return;
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
    case StmtIRKind::ExprStmt: {
      DAGNode* e = static_cast<ExprStmtIR*>(stmt)->expr;
      if (!e) break;
      if (e->kind == NodeKind::UnaryOp &&
          (e->name == "++" || e->name == "--") && !e->operands.empty()) {
        std::string root = lvalueRoot(e->operands[0]);
        if (!root.empty()) out.insert(root);
      } else if (e->kind == NodeKind::BinaryOp && e->op == '=' &&
                 e->operands.size() == 2) {
        std::string root = lvalueRoot(e->operands[0]);
        if (!root.empty()) out.insert(root);
      }
      break;
    }
    default:
      break;
  }
}

// Replace all Variable nodes with the given name in a DAG subtree.
// Returns the (possibly new) root of the subtree.
inline DAGNode* substitute(IRModule& mod, DAGNode* root,
                           const std::string& name, DAGNode* replacement) {
  if (!root) return nullptr;
  if (root->kind == NodeKind::Variable && root->name == name)
    return replacement;
  if (root->operands.empty()) return root;

  bool changed = false;
  std::vector<DAGNode*> newOps;
  for (auto* op : root->operands) {
    DAGNode* r = substitute(mod, op, name, replacement);
    newOps.push_back(r);
    if (r != op) changed = true;
  }
  if (!changed) return root;

  // Rebuild node through factory to get CSE dedup
  switch (root->kind) {
    case NodeKind::BinaryOp:
      return mod.createBinaryOp(root->op, newOps[0], newOps[1]);
    case NodeKind::UnaryOp: {
      DAGNode* u = mod.createUnaryOp(root->op, newOps[0]);
      if (!root->name.empty()) u->name = root->name;  // preserve ++ / --
      return u;
    }
    case NodeKind::ArrayAccess:
      return mod.createArrayAccess(newOps[0], newOps[1], root->pure);
    case NodeKind::MemberAccess:
      return mod.createMemberAccess(newOps[0], root->name, root->pure);
    case NodeKind::ArrowAccess:
      return mod.createArrowAccess(newOps[0], root->name, root->pure);
    case NodeKind::Call: {
      std::vector<DAGNode*> args(newOps.begin() + 1, newOps.end());
      return mod.createCall(newOps[0], args, root->pure);
    }
    case NodeKind::Cast: {
      DAGNode* node = mod.createNode(NodeKind::Cast);
      node->name = root->name;
      node->operands = newOps;
      return mod.findExistingNode(node);
    }
    case NodeKind::Ternary: {
      DAGNode* node = mod.createNode(NodeKind::Ternary);
      node->op = root->op;
      node->operands = newOps;
      return mod.findExistingNode(node);
    }
    default:
      return root;
  }
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
        case '/': result = (rhs->constVal != 0) ? lhs->constVal / rhs->constVal : 0; break;
        case 'e': result = (lhs->constVal == rhs->constVal) ? 1 : 0; break;
        case 'n': result = (lhs->constVal != rhs->constVal) ? 1 : 0; break;
        case '<': result = (lhs->constVal < rhs->constVal) ? 1 : 0; break;
        case '>': result = (lhs->constVal > rhs->constVal) ? 1 : 0; break;
        case 'l': result = (lhs->constVal <= rhs->constVal) ? 1 : 0; break;
        case 'g': result = (lhs->constVal >= rhs->constVal) ? 1 : 0; break;
        default: return node;
      }
      std::string text;
      if (result == std::floor(result) && std::fabs(result) < 1e15) {
        text = std::to_string(static_cast<long long>(result));
      } else {
        std::ostringstream oss;
        oss << std::setprecision(17) << result;
        text = oss.str();
      }
      return mod.createConst(result, text);
    }
  }
  return node;
}

}  // namespace cse
