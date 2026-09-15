#include "constant_fold.h"

#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"

namespace cse {

class ConstantFoldVisitor {
 public:
  explicit ConstantFoldVisitor(IRModule& mod) : module(mod) {}
  IRModule& module;
  int folds = 0;

  void visitStmt(StmtIR* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
      case StmtIRKind::Block: {
        auto* b = static_cast<BlockIR*>(stmt);
        for (auto& s : b->stmts) visitStmt(s.get());
        break;
      }
      case StmtIRKind::ForLoop: {
        auto* f = static_cast<ForLoopIR*>(stmt);
        visitStmt(f->init.get());
        f->cond = fold(f->cond);
        f->update = fold(f->update);
        if (f->updateRhs) f->updateRhs = fold(f->updateRhs);
        visitStmt(f->body.get());
        break;
      }
      case StmtIRKind::IfElse: {
        auto* ie = static_cast<IfElseIR*>(stmt);
        ie->cond = fold(ie->cond);
        visitStmt(ie->thenBranch.get());
        visitStmt(ie->elseBranch.get());
        break;
      }
      case StmtIRKind::ExprStmt:
        if (auto* e = static_cast<ExprStmtIR*>(stmt)->expr)
          static_cast<ExprStmtIR*>(stmt)->expr = fold(e);
        break;
      case StmtIRKind::Assign:
        if (auto* v = static_cast<AssignIR*>(stmt)->value)
          static_cast<AssignIR*>(stmt)->value = fold(v);
        break;
      case StmtIRKind::VarDecl:
        if (auto* i = static_cast<VarDeclIR*>(stmt)->init)
          static_cast<VarDeclIR*>(stmt)->init = fold(i);
        break;
      case StmtIRKind::Return:
        if (auto* v = static_cast<ReturnIR*>(stmt)->value)
          static_cast<ReturnIR*>(stmt)->value = fold(v);
        break;
    }
  }

  DAGNode* fold(DAGNode* node) {
    DAGNode* result = foldConst(module, node);
    if (result != node) folds++;
    return result;
  }
};

void ConstantFoldPass::run(IRModule& module) {
  ConstantFoldVisitor visitor(module);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
