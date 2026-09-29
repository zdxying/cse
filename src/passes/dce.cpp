#include "dce.h"

#include <unordered_map>
#include <unordered_set>

#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// (The impure-call test and the use count both come from ir_utils.h; they used
// to be private copies here and in loop_unroll, which is how they drifted.)

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
                    [&](DAGNode*& e) { countVarUses(e, uses); });
    if (pruneBlock(static_cast<BlockIR*>(module.body.get()), uses)) {
      continue;
    }
    break;
  }
}

}  // namespace cse
