#include "value_prop.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../analysis/effects.h"
#include "../analysis/purity.h"
#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/location.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// `isCheapShareableValue` (analysis/purity.h) owns the "cheap enough to inline
// at a use site" test. It used to be a private copy here -- and a *differently
// defined* copy of the same name in algebraic_simplify.cpp.

// Does `init` read only variables that are never reassigned in this function?
//
// The rewrite copies the initializer to the use site, so it stays valid only if
// every value it depends on is the same there as at the definition. Checking
// that the inlined name itself is never reassigned (done by the caller) is not
// enough: `double t = a; a = b; return t;` would otherwise become
// `a = b; return a;`. Requires that no variable read by `init` is written
// anywhere in the function.
bool initDepsStable(DAGNode* init, const LocationSet& reassigned,
                    const AliasInfo& aliases) {
  if (!init) return true;
  if (init->kind == NodeKind::Variable) {
    // `writeAffectsRead` is wildcard-aware and reference-aware: the wildcard an
    // impure call contributes says any global, and anything reachable from an
    // argument, may have been written, so no variable is stable across it; and a
    // read through a reference local is changed by a write to its referent.
    if (aliases.writeAffectsRead(reassigned, init->name)) return false;
    return true;
  }
  for (auto* op : init->operands) {
    if (!initDepsStable(op, reassigned, aliases)) return false;
  }
  return true;
}

// Substitute variables in a DAG subtree, rebuilding parent nodes as needed.
// The kind -> factory mapping lives in rebuildWithOperands (ir_utils.h); this
// function only owns the "when to rebuild" decision. It used to carry its own
// copy of the switch, which dropped the ++/-- spelling of a rebuilt unary and
// silently discarded a substitution into a cast or a conditional.
DAGNode* propExpr(DAGNode* e, IRModule& mod,
                  const std::unordered_map<std::string, DAGNode*>& defs) {
  if (!e) return nullptr;
  if (e->kind == NodeKind::Variable) {
    auto it = defs.find(e->name);
    return it != defs.end() ? it->second : e;
  }
  bool changed = false;
  std::vector<DAGNode*> newOps;
  newOps.reserve(e->operands.size());
  for (auto* op : e->operands) {
    DAGNode* r = propExpr(op, mod, defs);
    newOps.push_back(r);
    if (r != op) changed = true;
  }
  if (!changed) return e;
  return rebuildWithOperands(mod, e, newOps);
}

// Walk statements and collect the definitions that may be inlined.
//
// `blocked` = names that must not be inlined (parameters, plus anything
// written); `written` = names written anywhere, which an initializer may not
// read. A parameter that is never written is a perfectly stable source of
// values, so the two sets cannot be conflated.
//
// Only `VarDecl` initializers qualify. Plain assignments are deliberately left
// out: `toRemove` can only drop declarations, so inlining an assignment would
// leave the assignment behind while its uses were rewritten anyway -- and the
// assignment target is by definition a written name, so it can never satisfy
// `initDepsStable` either.
//
// A name that is the referent of a reference declaration (`T& r = name;`) is
// also left out: substituting its value into the binding would produce an
// ill-formed or semantically different reference (see
// AliasInfo::isAliasedReferent).
void walkStmt(StmtIR* stmt,
              const std::unordered_set<std::string>& blocked,
              const LocationSet& written, const AliasInfo& aliases,
              std::unordered_map<std::string, DAGNode*>& defs,
              std::vector<std::string>& toRemove) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) {
        if (s->kind == StmtIRKind::VarDecl) {
          auto* decl = static_cast<VarDeclIR*>(s.get());
          if (decl->init && isCheapShareableValue(decl->init) &&
              blocked.find(decl->name) == blocked.end() &&
              !aliases.isAliasedReferent(decl->name) &&
              initDepsStable(decl->init, written, aliases)) {
            defs[decl->name] = decl->init;
            toRemove.push_back(decl->name);
          }
        }
        walkStmt(s.get(), blocked, written, aliases, defs, toRemove);
      }
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      walkStmt(f->init.get(), blocked, written, aliases, defs, toRemove);
      walkStmt(f->body.get(), blocked, written, aliases, defs, toRemove);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      walkStmt(ie->thenBranch.get(), blocked, written, aliases, defs, toRemove);
      walkStmt(ie->elseBranch.get(), blocked, written, aliases, defs, toRemove);
      break;
    }
    // A declaration is only ever visited from the Block branch above, where its
    // sibling scope is known; these kinds own no nested statements.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
}

// Apply substitutions to all expressions in a statement tree.
void applyProp(StmtIR* stmt, IRModule& mod,
               const std::unordered_map<std::string, DAGNode*>& defs) {
  forEachExprDeep(stmt, [&](DAGNode*& e) { e = propExpr(e, mod, defs); });
}

// How many VarDecls does this statement tree contain? Used as the structural
// bound on ValueProp's fixpoint: each round either inlines something (removing
// at least one declaration) or exits, so the number of declarations bounds the
// number of productive rounds.
int countVarDecls(const StmtIR* stmt) {
  if (!stmt) return 0;
  int n = 0;
  switch (stmt->kind) {
    case StmtIRKind::VarDecl:
      return 1;
    case StmtIRKind::Block:
      for (const auto& s : static_cast<const BlockIR*>(stmt)->stmts)
        n += countVarDecls(s.get());
      return n;
    case StmtIRKind::ForLoop: {
      const auto* f = static_cast<const ForLoopIR*>(stmt);
      return countVarDecls(f->init.get()) + countVarDecls(f->body.get());
    }
    case StmtIRKind::IfElse: {
      const auto* ie = static_cast<const IfElseIR*>(stmt);
      return countVarDecls(ie->thenBranch.get()) +
             countVarDecls(ie->elseBranch.get());
    }
    // These kinds declare nothing.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::Return:
      return 0;
  }
  return 0;
}

// Remove inlined VarDecls from a block.
//
// A declaration is dropped only once its name has no surviving use. The
// substitution resolves one link per call, so when trivial definitions chain
// (`double u = t;` with `double t = a;`) `u` becomes `t` while `t` is still
// referenced -- erasing `t` here would emit a use of an undeclared variable
// (or, if an outer/global of the same name exists, silently bind to it). The
// name is re-checked against the already-rewritten body, so a declaration is
// kept until its last reference is gone; the outer iteration then removes it.
void removeInlined(StmtIR* stmt, const std::vector<std::string>& toRemove) {
  if (!stmt) return;
  if (stmt->kind != StmtIRKind::Block) return;
  std::unordered_set<std::string> referenced;
  forEachExprDeep(stmt, [&](DAGNode*& e) { collectReadNames(e, referenced); });
  auto* b = static_cast<BlockIR*>(stmt);
  auto it = b->stmts.begin();
  while (it != b->stmts.end()) {
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* decl = static_cast<VarDeclIR*>(it->get());
      bool inlined = false;
      for (auto& name : toRemove) {
        if (decl->name == name) { inlined = true; break; }
      }
      if (inlined && !referenced.count(decl->name)) {
        it = b->stmts.erase(it);
        continue;
      }
    }
    ++it;
  }
}

}  // namespace

void ValuePropPass::run(IRModule& module) {
  // Collect function parameter names — never inline these
  std::unordered_set<std::string> params;
  for (auto& p : module.funcSig.params) params.insert(p.name);

  // Pre-compute: every location written anywhere in the function, closed over
  // reference aliases (see ir/location.h) so a write through a reference local
  // also counts as a write of its referent. Used both to protect a variable
  // from being inlined itself and, via initDepsStable, to reject initializers
  // that read a variable which is written somewhere.
  AliasInfo aliases = AliasInfo::build(module.body.get());
  LocationSet reassigned = aliases.writtenBy(module.body.get());

  // The written *names*, for the by-name "must not inline" test. A wildcard
  // (an impure call) names no particular root; it is handled by
  // initDepsStable's `anything()` check instead.
  std::unordered_set<std::string> reassignedNames;
  for (const auto& l : reassigned.items()) reassignedNames.insert(l.root);

  // Iterate to a fixpoint. Each round resolves one link of a definition chain,
  // and `defs`/`toRemove` are recomputed every round from the already-shrunk
  // body, so the number of remaining inlinable declarations strictly decreases
  // whenever the loop does not exit. The round bound is therefore *structural*
  // -- the number of VarDecls in the function -- rather than a hand-picked
  // constant. The old cap of 5 silently stopped mid-chain: a 60-link chain kept
  // four declarations and a 120-link chain kept 63, so the output depended on
  // how many times the pipeline ran (a second `bin/cse` invocation finished the
  // job).
  const int maxRounds = 1 + countVarDecls(module.body.get());
  for (int iter = 0; iter < maxRounds; ++iter) {
    // Don't inline params or reassigned vars
    std::unordered_set<std::string> skip = reassignedNames;
    for (auto& p : params) skip.insert(p);

    // Collect trivial definitions
    std::unordered_map<std::string, DAGNode*> defs;
    std::vector<std::string> toRemove;
    walkStmt(module.body.get(), skip, reassigned, aliases, defs, toRemove);
    if (defs.empty()) break;

    applyProp(module.body.get(), module, defs);
    removeInlined(module.body.get(), toRemove);
  }
}

}  // namespace cse
