#include "dce.h"

#include <unordered_map>
#include <unordered_set>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

namespace {

// Does the expression contain any impure (side-effecting) call?
bool hasImpureCall(DAGNode* e) {
  if (!e) return false;
  if (e->kind == NodeKind::Call && !e->pure) return true;
  for (auto* op : e->operands)
    if (hasImpureCall(op)) return true;
  return false;
}

// Count variable uses in a DAG subtree (excludes the VarDecl name itself).
void countExprUses(DAGNode* e, std::unordered_map<std::string, int>& counts) {
  if (!e) return;
  if (e->kind == NodeKind::Variable) counts[e->name]++;
  for (auto* op : e->operands) countExprUses(op, counts);
}

// Count uses across a statement tree, skipping VarDecl name definitions.
void countStmtUses(StmtIR* stmt, std::unordered_map<std::string, int>& counts) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) countStmtUses(s.get(), counts);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      countStmtUses(f->init.get(), counts);
      countExprUses(f->cond, counts);
      countExprUses(f->update, counts);
      countExprUses(f->updateRhs, counts);
      countStmtUses(f->body.get(), counts);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      countExprUses(ie->cond, counts);
      countStmtUses(ie->thenBranch.get(), counts);
      countStmtUses(ie->elseBranch.get(), counts);
      break;
    }
    case StmtIRKind::ExprStmt:
      countExprUses(static_cast<ExprStmtIR*>(stmt)->expr, counts);
      break;
    case StmtIRKind::Assign:
      countExprUses(static_cast<AssignIR*>(stmt)->value, counts);
      break;
    case StmtIRKind::VarDecl:
      // Count uses in the initializer, but NOT the declaration name itself
      countExprUses(static_cast<VarDeclIR*>(stmt)->init, counts);
      break;
    case StmtIRKind::Return:
      countExprUses(static_cast<ReturnIR*>(stmt)->value, counts);
      break;
  }
}

// Remove dead statements from a block. Returns true if any were removed.
bool pruneBlock(BlockIR* block, const std::unordered_map<std::string, int>& uses) {
  bool changed = false;
  auto it = block->stmts.begin();
  while (it != block->stmts.end()) {
    bool remove = false;
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* decl = static_cast<VarDeclIR*>(it->get());
      // Remove uninitialized vars with no uses, or zero-init vars with no uses
      bool isZeroInit = !decl->init ||
                        (decl->init->kind == NodeKind::Constant && decl->init->constVal == 0);
      if (isZeroInit) {
        auto uit = uses.find(decl->name);
        if (uit == uses.end() || uit->second == 0) remove = true;
      }
    } else if ((*it)->kind == StmtIRKind::Assign) {
      auto* assign = static_cast<AssignIR*>(it->get());
      auto uit = uses.find(assign->target);
      // Only drop the assignment if the RHS has no side effects.
      if ((uit == uses.end() || uit->second == 0) && !hasImpureCall(assign->value))
        remove = true;
    }
    if (remove) {
      it = block->stmts.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  return changed;
}

}  // namespace

void DCEPass::run(IRModule& module) {
  // Iterate until no more dead code is found
  for (int iter = 0; iter < 10; ++iter) {
    std::unordered_map<std::string, int> uses;
    countStmtUses(module.body.get(), uses);
    if (pruneBlock(static_cast<BlockIR*>(module.body.get()), uses)) {
      continue;
    }
    break;
  }
}

}  // namespace cse
