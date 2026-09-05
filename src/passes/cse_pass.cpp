#include "cse_pass.h"

#include <unordered_map>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

// Visitor to traverse statements and replace duplicate DAG nodes
class CSEVisitor : public StmtIR {
 public:
  explicit CSEVisitor(IRModule& mod) : StmtIR(StmtIRKind::Block), module(mod) {}

  IRModule& module;
  int replaced = 0;

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
        if (forLoop->cond) forLoop->cond = simplify(forLoop->cond);
        if (forLoop->update) forLoop->update = simplify(forLoop->update);
        if (forLoop->updateRhs) forLoop->updateRhs = simplify(forLoop->updateRhs);
        visitStmt(forLoop->body.get());
        break;
      }
      case StmtIRKind::IfElse: {
        auto* ifElse = static_cast<IfElseIR*>(stmt);
        ifElse->cond = simplify(ifElse->cond);
        visitStmt(ifElse->thenBranch.get());
        visitStmt(ifElse->elseBranch.get());
        break;
      }
      case StmtIRKind::ExprStmt: {
        auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
        if (exprStmt->expr) exprStmt->expr = simplify(exprStmt->expr);
        break;
      }
      case StmtIRKind::Assign: {
        auto* assign = static_cast<AssignIR*>(stmt);
        if (assign->value) assign->value = simplify(assign->value);
        break;
      }
      case StmtIRKind::VarDecl: {
        auto* decl = static_cast<VarDeclIR*>(stmt);
        if (decl->init) decl->init = simplify(decl->init);
        break;
      }
      case StmtIRKind::Return: {
        auto* ret = static_cast<ReturnIR*>(stmt);
        if (ret->value) ret->value = simplify(ret->value);
        break;
      }
    }
  }

  DAGNode* simplify(DAGNode* node) {
    // For now, the CSE is already handled during DAG construction.
    // This pass can be extended for peephole optimizations.
    return node;
  }
};

void CSEPass::run(IRModule& module) {
  CSEVisitor visitor(module);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
