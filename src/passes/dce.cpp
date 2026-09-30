#include "dce.h"

#include <unordered_map>
#include <unordered_set>

#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// (The side-effect test and the use count both come from ir_utils.h; they used
// to be private copies here and in loop_unroll, which is how they drifted.)

// Every name the function introduces itself: parameters plus local declarations.
//
// Dropping a store to one of these can only throw away work nothing can observe.
// A store to any *other* name cannot be judged that way -- the region never
// declares it, so it names storage that outlives the call, and the write is
// still visible after the function returns:
//
//   double g = 0.0;
//   //@cse
//   double f(double a) { g = a; return a; }   // `g = a;` used to be deleted
//   //@cse
//
// "nothing in this function reads it back" is not the same as "nothing can see
// it".
void collectDeclared(const StmtIR* stmt, std::unordered_set<std::string>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::VarDecl:
      out.insert(static_cast<const VarDeclIR*>(stmt)->name);
      return;
    case StmtIRKind::Block:
      for (const auto& s : static_cast<const BlockIR*>(stmt)->stmts)
        collectDeclared(s.get(), out);
      return;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<const ForLoopIR*>(stmt);
      collectDeclared(f->init.get(), out);
      collectDeclared(f->body.get(), out);
      return;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<const IfElseIR*>(stmt);
      collectDeclared(ie->thenBranch.get(), out);
      collectDeclared(ie->elseBranch.get(), out);
      return;
    }
    default:
      return;
  }
}

std::unordered_set<std::string> ownedLocals(const IRModule& module) {
  std::unordered_set<std::string> owned;
  for (const auto& p : module.funcSig.params) owned.insert(p.name);
  collectDeclared(module.body.get(), owned);
  return owned;
}

// Remove dead statements from a block. Returns true if any were removed.
//
// `reads` counts variable *occurrences*, so it says nothing about a name that is
// only ever stored into. That distinction matters in both directions and both
// are handled below: a store is kept when its value has an effect, and the
// declaration it writes through is kept whenever anything still writes it --
// otherwise the surviving store would refer to a variable that no longer exists.
bool pruneBlock(BlockIR* block,
                const std::unordered_map<std::string, int>& reads,
                const std::unordered_set<std::string>& written,
                const std::unordered_set<std::string>& owned) {
  bool changed = false;
  auto it = block->stmts.begin();
  while (it != block->stmts.end()) {
    bool remove = false;
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* decl = static_cast<VarDeclIR*>(it->get());
      // An uninitialized or zero-initialized local that is neither read nor
      // written, and whose initializer has no effect of its own.
      bool isZeroInit = !decl->init ||
                        (decl->init->kind == NodeKind::Constant && decl->init->constVal == 0);
      auto rit = reads.find(decl->name);
      if (isZeroInit && !hasSideEffect(decl->init) &&
          (rit == reads.end() || rit->second == 0) &&
          !written.count(decl->name))
        remove = true;
    } else if ((*it)->kind == StmtIRKind::Assign) {
      auto* assign = static_cast<AssignIR*>(it->get());
      // Three independent reasons to keep a store; each was a wrong-code bug of
      // its own before:
      //   * a complex lvalue (`a[i] = v;`, `p->f = v;`) writes through memory,
      //     and its lvalue is not a name that could be reasoned about;
      //   * the target is not a local this function declares, so it may name a
      //     global, where the write outlives the call;
      //   * the value has an effect even though its result is discarded:
      //     `unused = y++;` increments `y`, and deleting the statement deleted
      //     the increment with it -- the function returned `a` instead of
      //     `a + 1`, silently.
      auto rit = reads.find(assign->target);
      if (!assign->targetExpr && owned.count(assign->target) &&
          (rit == reads.end() || rit->second == 0) &&
          !hasSideEffect(assign->value))
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

  // Which names a store could be discarded from at all. Computed once: pruning
  // never introduces a declaration.
  const std::unordered_set<std::string> owned = ownedLocals(module);

  // Iterate until no more dead code is found. `reads` and `written` are
  // recomputed each round, so a declaration blocked only by a store that this
  // round removes is freed in the next one and the two converge.
  for (int iter = 0; iter < 10; ++iter) {
    std::unordered_map<std::string, int> reads;
    forEachExprDeep(module.body.get(),
                    [&](DAGNode*& e) { countVarUses(e, reads); });
    std::unordered_set<std::string> written;
    collectWrittenNames(module.body.get(), written);
    if (pruneBlock(static_cast<BlockIR*>(module.body.get()), reads, written,
                   owned)) {
      continue;
    }
    break;
  }
}

}  // namespace cse
