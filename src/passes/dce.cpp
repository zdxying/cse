#include "dce.h"

#include <unordered_map>
#include <unordered_set>

#include "../analysis/effects.h"
#include "../analysis/purity.h"
#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/location.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// (The side-effect test, the use count and the owned-storage test all come from
// the shared modules; they used to be private copies here and in loop_unroll,
// which is how they drifted.)

// Every name the function introduces itself *as private storage*: value
// parameters plus local declarations.
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
//
// A *reference* is not private storage, whether it arrives as a parameter or as
// a local declaration: `void setv(double& v) { v = 1.0; }` writes the caller's
// object and `double& r = v; r = 1.0;` writes whatever `r` binds. Both were
// treated as owned, so the store looked dead and was deleted. A pointer is
// different -- reassigning the pointer itself only changes the local copy, so
// it stays owned. `classifyDeclaredType` (ir/location.h) owns the classification;
// only the *reference* class names storage this function does not own.
bool isPrivateStorage(const std::string& type) {
  return classifyDeclaredType(type) != AliasClass::Reference;
}

void collectDeclared(const StmtIR* stmt, std::unordered_set<std::string>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::VarDecl: {
      const auto* d = static_cast<const VarDeclIR*>(stmt);
      if (isPrivateStorage(d->type)) out.insert(d->name);
      return;
    }
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
    // These kinds declare nothing. Listed rather than defaulted so that a new
    // StmtIRKind is a compile error here instead of silently doing nothing (see
    // the note on StmtIRKind in statement.h).
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::Return:
      return;
  }
}

// How many statements does this tree contain? The structural bound for DCE's
// fixpoint: every productive round removes at least one statement.
int countStmts(const StmtIR* stmt) {
  if (!stmt) return 0;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      int n = 1;
      for (const auto& s : static_cast<const BlockIR*>(stmt)->stmts)
        n += countStmts(s.get());
      return n;
    }
    case StmtIRKind::ForLoop: {
      const auto* f = static_cast<const ForLoopIR*>(stmt);
      return 1 + countStmts(f->init.get()) + countStmts(f->body.get());
    }
    case StmtIRKind::IfElse: {
      const auto* ie = static_cast<const IfElseIR*>(stmt);
      return 1 + countStmts(ie->thenBranch.get()) +
             countStmts(ie->elseBranch.get());
    }
    // A leaf statement.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      return 1;
  }
  return 0;
}

std::unordered_set<std::string> ownedLocals(const IRModule& module) {
  std::unordered_set<std::string> owned;
  for (const auto& p : module.funcSig.params) {
    // A reference parameter names the caller's object, not a local copy: a
    // store through it is observable after the call returns. A pointer
    // parameter is a value parameter for this purpose.
    if (isPrivateStorage(p.type)) owned.insert(p.name);
  }
  collectDeclared(module.body.get(), owned);
  return owned;
}

// Recurse into a nested statement so its block gets the same pruning. Only the
// block shapes carry statements; a loop's init shares the enclosing scope and
// its body is a block of its own.
bool pruneBlock(BlockIR* block,
                const std::unordered_map<std::string, int>& reads,
                const LocationSet& written,
                const std::unordered_set<std::string>& owned);

bool pruneNested(StmtIR* s,
                 const std::unordered_map<std::string, int>& reads,
                 const LocationSet& written,
                 const std::unordered_set<std::string>& owned) {
  if (!s) return false;
  switch (s->kind) {
    case StmtIRKind::Block:
      return pruneBlock(static_cast<BlockIR*>(s), reads, written, owned);
    case StmtIRKind::ForLoop:
      return pruneNested(static_cast<ForLoopIR*>(s)->body.get(), reads, written,
                         owned);
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(s);
      bool changed = pruneNested(ie->thenBranch.get(), reads, written, owned);
      if (pruneNested(ie->elseBranch.get(), reads, written, owned)) changed = true;
      return changed;
    }
    // These kinds introduce no nested scope. Listed explicitly so a new
    // StmtIRKind is a compile error here rather than a silently missed scope.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      return false;
  }
  return false;
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
                const LocationSet& written,
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
      if (isZeroInit && !hasObservableEffect(decl->init) &&
          (rit == reads.end() || rit->second == 0) &&
          !written.containsRoot(decl->name))
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
          !hasObservableEffect(assign->value))
        remove = true;
    }
    if (remove) {
      it = block->stmts.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Recurse into nested scopes. A nested `if` / `for` body / bare `{ }` is a
  // separate block whose own statements were never visited: a dead declaration
  // or a dead store to a private local inside one survived every round. Cleanup
  // already recurses (`pruneNested`); DCE did not, so the two disagreed about
  // what "dead code" means.
  //
  // `reads` and `written` are whole-function sets, so reusing them for a nested
  // block is conservative: a declaration is removed only when nothing anywhere
  // in the function reads it and nothing writes it. Names are alpha-renamed per
  // scope by the builder, so two scopes cannot share one by accident.
  for (auto& s : block->stmts) {
    if (pruneNested(s.get(), reads, written, owned)) changed = true;
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

  // Reference aliases for the whole function: a write through a reference local
  // also writes its referent (ir/location.h).
  AliasInfo aliases = AliasInfo::build(module.body.get());

  // Iterate until no more dead code is found. `reads` and `written` are
  // recomputed each round, so a declaration blocked only by a store that this
  // round removes is freed in the next one and the two converge.
  //
  // The bound is structural, not a magic constant: every productive round
  // removes at least one statement, so the statement count bounds the rounds.
  // The old cap of 10 could stop early on a long dead chain, leaving work that
  // a second run would finish (the output then depended on how many times the
  // pipeline ran).
  const int maxRounds = 1 + countStmts(module.body.get());
  for (int iter = 0; iter < maxRounds; ++iter) {
    std::unordered_map<std::string, int> reads;
    forEachExprDeep(module.body.get(),
                    [&](DAGNode*& e) { countReadNames(e, reads); });
    LocationSet written = aliases.writtenBy(module.body.get());
    if (pruneBlock(static_cast<BlockIR*>(module.body.get()), reads, written,
                   owned)) {
      continue;
    }
    break;
  }
}

}  // namespace cse
