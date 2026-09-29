#include "dce.h"

#include <unordered_map>
#include <unordered_set>

#include "../ir/ir_module.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

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
      // Complex lvalues (array/member stores) are effects: never drop them.
      auto uit = uses.find(assign->target);
      if (!assign->targetExpr && (uit == uses.end() || uit->second == 0) &&
          !hasImpureCall(assign->value))
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
  // Only a top-level block is pruned. IRBuilder guarantees that shape, but the
  // cast below would be undefined behaviour on anything else, so check it.
  if (!module.body || module.body->kind != StmtIRKind::Block) return;

  // Iterate until no more dead code is found
  for (int iter = 0; iter < 10; ++iter) {
    std::unordered_map<std::string, int> uses;
    forEachExprDeep(module.body.get(),
                    [&](DAGNode*& e) { countExprUses(e, uses); });
    if (pruneBlock(static_cast<BlockIR*>(module.body.get()), uses)) {
      continue;
    }
    break;
  }
}

}  // namespace cse
