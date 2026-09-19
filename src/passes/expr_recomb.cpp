#include "expr_recomb.h"

#include <iostream>
#include <unordered_set>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

class ExprRecombineVisitor {
 public:
  explicit ExprRecombineVisitor(IRModule& mod) : module(mod) {}

  IRModule& module;
  int rewrites = 0;

  void visitStmt(StmtIR* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
      case StmtIRKind::Block: {
        auto* block = static_cast<BlockIR*>(stmt);
        for (auto& s : block->stmts) visitStmt(s.get());
        break;
      }
      case StmtIRKind::ForLoop: {
        auto* forLoop = static_cast<ForLoopIR*>(stmt);
        visitStmt(forLoop->init.get());
        forLoop->cond = rewrite(forLoop->cond);
        forLoop->update = rewrite(forLoop->update);
        if (forLoop->updateRhs) forLoop->updateRhs = rewrite(forLoop->updateRhs);
        visitStmt(forLoop->body.get());
        break;
      }
      case StmtIRKind::IfElse: {
        auto* ifElse = static_cast<IfElseIR*>(stmt);
        ifElse->cond = rewrite(ifElse->cond);
        visitStmt(ifElse->thenBranch.get());
        visitStmt(ifElse->elseBranch.get());
        break;
      }
      case StmtIRKind::ExprStmt: {
        auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
        if (exprStmt->expr) exprStmt->expr = rewrite(exprStmt->expr);
        break;
      }
      case StmtIRKind::Assign: {
        auto* assign = static_cast<AssignIR*>(stmt);
        if (assign->value) assign->value = rewrite(assign->value);
        break;
      }
      case StmtIRKind::VarDecl: {
        auto* decl = static_cast<VarDeclIR*>(stmt);
        if (decl->init) decl->init = rewrite(decl->init);
        break;
      }
      case StmtIRKind::Return: {
        auto* ret = static_cast<ReturnIR*>(stmt);
        if (ret->value) ret->value = rewrite(ret->value);
        break;
      }
    }
  }

  DAGNode* rewrite(DAGNode* node) {
    if (!node) return nullptr;

    // First, recursively rewrite children. Rebuild through the factory when a
    // child changes so the node stays registered under a consistent hash.
    if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
      DAGNode* lhs = rewrite(node->operands[0]);
      DAGNode* rhs = rewrite(node->operands[1]);
      if (lhs != node->operands[0] || rhs != node->operands[1]) {
        node = module.createBinaryOp(node->op, lhs, rhs);
      }
    }

    // Try to apply recombination patterns
    if (node->kind == NodeKind::BinaryOp && (node->op == '+' || node->op == '-')) {
      DAGNode* result = tryFactorAddSub(node);
      if (result && result != node) {
        rewrites++;
        return result;
      }
    }

    return node;
  }

 private:
  bool sameExpr(DAGNode* a, DAGNode* b) {
    if (!a || !b) return false;
    // Compare by ID (same node in DAG) or by structural equality
    if (a->id == b->id) return true;
    if (a->kind != b->kind) return false;
    if (a->op != b->op) return false;
    if (a->operands.size() != b->operands.size()) return false;
    if (a->kind == NodeKind::Constant && a->constVal != b->constVal) return false;
    if (a->kind == NodeKind::Variable && a->name != b->name) return false;
    if (a->kind == NodeKind::MemberAccess && a->name != b->name) return false;
    if (a->kind == NodeKind::ArrowAccess && a->name != b->name) return false;
    for (size_t i = 0; i < a->operands.size(); i++) {
      if (!sameExpr(a->operands[i], b->operands[i])) return false;
    }
    return true;
  }

  DAGNode* tryFactorAddSub(DAGNode* addNode) {
    DAGNode* lhs = addNode->operands[0];
    DAGNode* rhs = addNode->operands[1];

    if (lhs->kind != NodeKind::BinaryOp || rhs->kind != NodeKind::BinaryOp)
      return addNode;

    char addOp = addNode->op;

    // Pattern: a*x ± a*y → a*(x ± y) and a*x ± b*x → (a ± b)*x
    if ((lhs->op == '*' || lhs->op == '/') &&
        (rhs->op == '*' || rhs->op == '/') &&
        lhs->operands.size() == 2 && rhs->operands.size() == 2) {
      DAGNode* la = lhs->operands[0];
      DAGNode* lb = lhs->operands[1];
      DAGNode* ra = rhs->operands[0];
      DAGNode* rb = rhs->operands[1];

      // a*x ± a*y → a*(x ± y)
      if (sameExpr(la, ra)) {
        auto inner = module.createBinaryOp(addOp, lb, rb);
        return module.createBinaryOp('*', la, inner);
      }
      if (sameExpr(lb, rb)) {
        auto outer = module.createBinaryOp(addOp, la, ra);
        return module.createBinaryOp('*', outer, lb);
      }
      if (sameExpr(la, rb)) {
        auto inner = module.createBinaryOp(addOp, lb, ra);
        return module.createBinaryOp('*', la, inner);
      }
      if (sameExpr(lb, ra)) {
        auto outer = module.createBinaryOp(addOp, la, rb);
        return module.createBinaryOp('*', outer, lb);
      }
    }

    // Pattern: a*x + a → a*(x + 1)
    if (addOp == '+' && lhs->kind == NodeKind::BinaryOp && lhs->op == '*' &&
        lhs->operands.size() == 2) {
      DAGNode* mulA = lhs->operands[0];
      DAGNode* mulB = lhs->operands[1];
      if (sameExpr(mulA, rhs)) {
        auto one = module.createConst(1, "1");
        auto inner = module.createBinaryOp('+', mulB, one);
        return module.createBinaryOp('*', mulA, inner);
      }
      if (sameExpr(mulB, rhs)) {
        auto one = module.createConst(1, "1");
        auto inner = module.createBinaryOp('+', mulA, one);
        return module.createBinaryOp('*', mulB, inner);
      }
    }

    // Pattern: a*x - a → a*(x - 1)
    if (addOp == '-' && lhs->kind == NodeKind::BinaryOp && lhs->op == '*' &&
        lhs->operands.size() == 2) {
      DAGNode* mulA = lhs->operands[0];
      DAGNode* mulB = lhs->operands[1];
      if (sameExpr(mulA, rhs)) {
        auto one = module.createConst(1, "1");
        auto inner = module.createBinaryOp('-', mulB, one);
        return module.createBinaryOp('*', mulA, inner);
      }
      if (sameExpr(mulB, rhs)) {
        auto one = module.createConst(1, "1");
        auto inner = module.createBinaryOp('-', mulA, one);
        return module.createBinaryOp('*', mulB, inner);
      }
    }

    return addNode;
  }
};

void ExprRecombinePass::run(IRModule& module) {
  ExprRecombineVisitor visitor(module);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
