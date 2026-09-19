#pragma once
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

#include "dag_node.h"
#include "ir_module.h"
#include "statement.h"

namespace cse {

// Count how many times each variable name is used across a StmtIR tree.
inline std::unordered_map<std::string, int> countUses(StmtIR* root);

namespace detail {

inline void countUsesExpr(DAGNode* e,
                          std::unordered_map<std::string, int>& counts) {
  if (!e) return;
  if (e->kind == NodeKind::Variable) counts[e->name]++;
  for (auto* op : e->operands) countUsesExpr(op, counts);
}

inline void countUsesStmt(StmtIR* stmt,
                          std::unordered_map<std::string, int>& counts) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) countUsesStmt(s.get(), counts);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      countUsesStmt(f->init.get(), counts);
      countUsesExpr(f->cond, counts);
      countUsesExpr(f->update, counts);
      countUsesExpr(f->updateRhs, counts);
      countUsesStmt(f->body.get(), counts);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      countUsesExpr(ie->cond, counts);
      countUsesStmt(ie->thenBranch.get(), counts);
      countUsesStmt(ie->elseBranch.get(), counts);
      break;
    }
    case StmtIRKind::ExprStmt:
      countUsesExpr(static_cast<ExprStmtIR*>(stmt)->expr, counts);
      break;
    case StmtIRKind::Assign:
      countUsesExpr(static_cast<AssignIR*>(stmt)->value, counts);
      break;
    case StmtIRKind::VarDecl:
      countUsesExpr(static_cast<VarDeclIR*>(stmt)->init, counts);
      break;
    case StmtIRKind::Return:
      countUsesExpr(static_cast<ReturnIR*>(stmt)->value, counts);
      break;
  }
}

}  // namespace detail

inline std::unordered_map<std::string, int> countUses(StmtIR* root) {
  std::unordered_map<std::string, int> counts;
  detail::countUsesStmt(root, counts);
  return counts;
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
    case NodeKind::UnaryOp:
      return mod.createUnaryOp(root->op, newOps[0]);
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
    default:
      return root;
  }
}

// Constant folding: if a BinaryOp has two Constant operands, compute the result.
inline DAGNode* foldConst(IRModule& mod, DAGNode* node) {
  if (!node) return nullptr;
  if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
    node->operands[0] = foldConst(mod, node->operands[0]);
    node->operands[1] = foldConst(mod, node->operands[1]);
    auto* l = node->operands[0];
    auto* r = node->operands[1];
    if (l->kind == NodeKind::Constant && r->kind == NodeKind::Constant) {
      double result = 0;
      switch (node->op) {
        case '+': result = l->constVal + r->constVal; break;
        case '-': result = l->constVal - r->constVal; break;
        case '*': result = l->constVal * r->constVal; break;
        case '/': result = (r->constVal != 0) ? l->constVal / r->constVal : 0; break;
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
    node->recomputeHash();
  }
  return node;
}

}  // namespace cse
