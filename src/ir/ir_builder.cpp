#include "ir_builder.h"

#include <sstream>
#include <stdexcept>

namespace cse {

IRBuilder::IRBuilder(IRModule* module) : _module(module) {}

void IRBuilder::buildFunction(const FunctionDef& func) {
  _module->funcSig.returnType = func.returnType;
  _module->funcSig.name = func.name;
  for (const auto& p : func.params) {
    _module->funcSig.params.push_back({p.type, p.name});
  }
  _module->body = buildStmt(*func.body);
}

std::unique_ptr<StmtIR> IRBuilder::buildStmt(const Stmt& stmt) {
  switch (stmt.kind) {
    case StmtKind::Block: {
      auto block = std::make_unique<BlockIR>();
      for (const auto& s : stmt.stmts) {
        block->stmts.push_back(buildStmt(*s));
      }
      return block;
    }
    case StmtKind::ForLoop: {
      auto forIR = std::make_unique<ForLoopIR>();
      if (stmt.forInit) forIR->init = buildStmt(*stmt.forInit);
      if (stmt.forCond) forIR->cond = buildExpr(*stmt.forCond);
      if (stmt.forUpdate) {
        // Try to parse update as simple: var++, var--, var+=expr, var=var+expr
        if (stmt.forUpdate->kind == ExprKind::PostfixOp) {
          forIR->updateOp = stmt.forUpdate->op;
          forIR->update = buildExpr(*stmt.forUpdate->operand);
        } else if (stmt.forUpdate->kind == ExprKind::BinaryOp && stmt.forUpdate->lhs &&
                   stmt.forUpdate->lhs->kind == ExprKind::Variable) {
          forIR->update = buildExpr(*stmt.forUpdate->lhs);
          forIR->updateOp = stmt.forUpdate->op;
          if (stmt.forUpdate->rhs) {
            forIR->updateRhs = buildExpr(*stmt.forUpdate->rhs);
          }
        } else {
          forIR->update = buildExpr(*stmt.forUpdate);
        }
      }
      forIR->body = buildStmt(*stmt.forBody);
      return forIR;
    }
    case StmtKind::IfElse: {
      auto ifIR = std::make_unique<IfElseIR>();
      ifIR->cond = buildExpr(*stmt.ifCond);
      ifIR->thenBranch = buildStmt(*stmt.ifThen);
      if (stmt.ifElse) ifIR->elseBranch = buildStmt(*stmt.ifElse);
      return ifIR;
    }
    case StmtKind::VarDecl: {
      auto decl = std::make_unique<VarDeclIR>();
      decl->type = stmt.varType;
      decl->name = stmt.varName;
      if (stmt.init) decl->init = buildExpr(*stmt.init);
      // Register variable in module
      _module->getVar(stmt.varName);
      return decl;
    }
    case StmtKind::Return: {
      auto ret = std::make_unique<ReturnIR>();
      if (stmt.retExpr) ret->value = buildExpr(*stmt.retExpr);
      return ret;
    }
    case StmtKind::Assignment: {
      auto assign = std::make_unique<AssignIR>();
      assign->target = stmt.varName;
      if (stmt.rhs) assign->value = buildExpr(*stmt.rhs);
      return assign;
    }
    case StmtKind::ExprStmt: {
      auto exprStmt = std::make_unique<ExprStmtIR>();
      if (stmt.expr) exprStmt->expr = buildExpr(*stmt.expr);
      return exprStmt;
    }
  }
  return nullptr;
}

DAGNode* IRBuilder::buildExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Number:
      return _module->createConst(expr.numVal, expr.numText);

    case ExprKind::Variable:
      return _module->getVar(expr.name);

    case ExprKind::BinaryOp:
      return buildBinaryOp(expr);

    case ExprKind::UnaryOp:
      return buildUnaryOp(expr);

    case ExprKind::ArrayAccess:
      return buildArrayAccess(expr);

    case ExprKind::MemberAccess: {
      DAGNode* base = buildExpr(*expr.base);
      return _module->createMemberAccess(base, expr.memberName);
    }

    case ExprKind::ArrowAccess: {
      DAGNode* base = buildExpr(*expr.base);
      return _module->createArrowAccess(base, expr.memberName);
    }

    case ExprKind::Call:
      return buildCall(expr);

    case ExprKind::Ternary: {
      DAGNode* cond = buildExpr(*expr.cond);
      DAGNode* trueExpr = buildExpr(*expr.trueExpr);
      DAGNode* falseExpr = buildExpr(*expr.falseExpr);
      auto node = _module->createNode(NodeKind::Ternary);
      node->op = '?';
      node->operands = {cond, trueExpr, falseExpr};
      return _module->findExistingNode(node);
    }

    case ExprKind::Cast: {
      DAGNode* operand = buildExpr(*expr.operand);
      auto node = _module->createNode(NodeKind::Cast);
      node->name = expr.castType;
      node->operands = {operand};
      return _module->findExistingNode(node);
    }

    case ExprKind::PostfixOp: {
      DAGNode* operand = buildExpr(*expr.operand);
      auto node = _module->createNode(NodeKind::UnaryOp);
      node->op = expr.op;
      node->operands = {operand};
      node->name = "postfix";
      return _module->findExistingNode(node);
    }
  }
  return nullptr;
}

DAGNode* IRBuilder::buildBinaryOp(const Expr& expr) {
  DAGNode* lhs = buildExpr(*expr.lhs);
  DAGNode* rhs = buildExpr(*expr.rhs);
  return _module->createBinaryOp(expr.op, lhs, rhs);
}

DAGNode* IRBuilder::buildUnaryOp(const Expr& expr) {
  DAGNode* operand = buildExpr(*expr.operand);
  return _module->createUnaryOp(expr.op, operand);
}

DAGNode* IRBuilder::buildArrayAccess(const Expr& expr) {
  DAGNode* base = buildExpr(*expr.base);
  DAGNode* result = base;
  for (const auto& idx : expr.indices) {
    DAGNode* index = buildExpr(*idx);
    result = _module->createArrayAccess(result, index);
  }
  return result;
}

DAGNode* IRBuilder::buildCall(const Expr& expr) {
  DAGNode* callee = buildExpr(*expr.base);
  std::vector<DAGNode*> args;
  for (const auto& arg : expr.callArgs) {
    args.push_back(buildExpr(*arg));
  }
  return _module->createCall(callee, args);
}

}  // namespace cse
