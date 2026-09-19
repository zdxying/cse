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

// Trip count of a canonical counted loop `for (i = 0; i < N; ++i)`, or 0 if
// the loop does not match (in which case the body is counted once).
int loopTripCount(ForLoopIR* f) {
  if (!f || !f->init || f->init->kind != StmtIRKind::VarDecl) return 0;
  auto* d = static_cast<VarDeclIR*>(f->init.get());
  if (d->name.empty()) return 0;
  if (d->init && !(d->init->kind == NodeKind::Constant && d->init->constVal == 0))
    return 0;

  if (!f->cond || f->cond->kind != NodeKind::BinaryOp || f->cond->op != '<' ||
      f->cond->operands.size() != 2)
    return 0;
  DAGNode* lhs = f->cond->operands[0];
  DAGNode* rhs = f->cond->operands[1];
  if (!(lhs->kind == NodeKind::Variable && lhs->name == d->name)) return 0;
  if (rhs->kind != NodeKind::Constant) return 0;
  int n = static_cast<int>(rhs->constVal);
  if (n <= 0) return 0;

  bool updateOk = false;
  if (f->update && f->update->kind == NodeKind::UnaryOp && f->update->name == "++" &&
      !f->update->operands.empty() && f->update->operands[0]->kind == NodeKind::Variable &&
      f->update->operands[0]->name == d->name) {
    updateOk = true;
  } else if (f->update && f->update->kind == NodeKind::Variable &&
             f->update->name == d->name && f->updateOp == '+') {
    updateOk = !f->updateRhs || (f->updateRhs->kind == NodeKind::Constant &&
                                 f->updateRhs->constVal == 1);
  }
  return updateOk ? n : 0;
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
      int trip = loopTripCount(f);
      if (trip > 0) {
        // Count the body once per iteration to reflect the rolled loop's cost.
        CostResult bodyRes;
        analyzeStmt(f->body.get(), visited, bodyRes);
        result.flops += bodyRes.flops * trip;
        result.totalNodes += bodyRes.totalNodes * trip;
        result.stmts += bodyRes.stmts * trip;
        result.vars += bodyRes.vars * trip;
      } else {
        analyzeStmt(f->body.get(), visited, result);
      }
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
