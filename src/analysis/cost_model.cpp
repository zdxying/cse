#include "cost_model.h"

#include <unordered_set>

#include "ir/dag_node.h"
#include "ir/ir_module.h"
#include "ir/statement.h"

namespace cse {

namespace {

bool isFlop(char op) {
  return op == '+' || op == '-' || op == '*' || op == '/';
}

void analyzeExpr(DAGNode* node, std::unordered_set<DAGNode*>& visited,
                 CostResult& result) {
  if (!node) return;
  if (visited.count(node)) return;
  visited.insert(node);

  result.totalNodes++;

  if (node->kind == NodeKind::BinaryOp && isFlop(node->op)) {
    result.flops++;
  }

  for (auto* op : node->operands) {
    analyzeExpr(op, visited, result);
  }
}

void analyzeStmt(StmtIR* stmt, std::unordered_set<DAGNode*>& visited,
                 CostResult& result) {
  if (!stmt) return;
  result.stmts++;

  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) analyzeStmt(s.get(), visited, result);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* v = static_cast<VarDeclIR*>(stmt);
      result.vars++;
      visited.clear();
      analyzeExpr(v->init, visited, result);
      break;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      visited.clear();
      analyzeExpr(a->value, visited, result);
      break;
    }
    case StmtIRKind::ExprStmt: {
      auto* e = static_cast<ExprStmtIR*>(stmt);
      visited.clear();
      analyzeExpr(e->expr, visited, result);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      analyzeStmt(f->init.get(), visited, result);
      visited.clear();
      analyzeExpr(f->cond, visited, result);
      visited.clear();
      analyzeExpr(f->update, visited, result);
      visited.clear();
      analyzeExpr(f->updateRhs, visited, result);
      analyzeStmt(f->body.get(), visited, result);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      visited.clear();
      analyzeExpr(ie->cond, visited, result);
      analyzeStmt(ie->thenBranch.get(), visited, result);
      analyzeStmt(ie->elseBranch.get(), visited, result);
      break;
    }
    case StmtIRKind::Return: {
      auto* r = static_cast<ReturnIR*>(stmt);
      visited.clear();
      analyzeExpr(r->value, visited, result);
      break;
    }
  }
}

}  // namespace

CostResult analyzeCost(IRModule& module) {
  CostResult result;
  std::unordered_set<DAGNode*> visited;
  analyzeStmt(module.body.get(), visited, result);
  return result;
}

}  // namespace cse
