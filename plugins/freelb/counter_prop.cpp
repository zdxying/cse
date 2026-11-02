#include "counter_prop.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "analysis/effects.h"
#include "ir/ir_module.h"
#include "ir/ir_utils.h"
#include "ir/location.h"
#include "ir/statement.h"

namespace cse {

namespace {

using ConstMap = std::unordered_map<std::string, double>;
using VectorLocalName = CounterPropPass::VectorLocalName;

// Context shared while rewriting a block: the known constant counters plus the
// information needed to resolve lowered vector locals.
struct Ctx {
  IRModule& mod;
  const ConstMap& known;
  const std::unordered_set<std::string>& names;
  const VectorLocalName& vecLocal;
  const AliasInfo& aliases;
};

DAGNode* rewriteIndexes(DAGNode* n, const Ctx& ctx) {
  if (!n) return n;
  bool changed = false;
  std::vector<DAGNode*> ops;
  ops.reserve(n->operands.size());
  for (auto* op : n->operands) {
    DAGNode* r = rewriteIndexes(op, ctx);
    ops.push_back(r);
    if (r != op) changed = true;
  }
  if (n->kind == NodeKind::ArrayAccess && ops.size() == 2 &&
      ops[1]->kind == NodeKind::Variable) {
    auto it = ctx.known.find(ops[1]->name);
    if (it != ctx.known.end()) {
      ops[1] = ctx.mod.createConst(it->second);
      changed = true;
    }
  }
  // Lowered vector local indexed by a now-constant index: `v[1]` -> `v_1`.
  if (n->kind == NodeKind::ArrayAccess && ops.size() == 2 &&
      ops[0]->kind == NodeKind::Variable && ops[1]->kind == NodeKind::Constant &&
      ctx.vecLocal) {
    std::string comp =
        ctx.vecLocal(ops[0]->name, static_cast<long long>(ops[1]->constVal));
    if (!comp.empty() && ctx.names.count(comp)) return ctx.mod.getVar(comp);
  }
  if (!changed) return n;
  // Shared kind -> factory mapping (ir_utils.h); a private copy here had drifted.
  return rebuildWithOperands(ctx.mod, n, ops);
}

// Rewrite the expression slots owned by `stmt` itself. Nested scopes (a Block,
// a loop body, an `if` branch) are deliberately NOT descended into here: they
// are separate scopes whose counters must be tracked from their own entry
// state, which `recurseNested` / `processScope` do with an explicit seed.
// Descending with the enclosing `known` would fold an index using a counter
// value a nested write has already invalidated -- `i = 0; for (...) { t[i] = a;
// ++i; }` folded `t[i]` to `t[0]` even though `i` changes every iteration.
void rewriteStmt(StmtIR* stmt, const Ctx& ctx) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block:
      break;  // nested scope, handled by recurseNested
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      rewriteStmt(f->init.get(), ctx);
      if (f->cond) f->cond = rewriteIndexes(f->cond, ctx);
      if (f->update) f->update = rewriteIndexes(f->update, ctx);
      if (f->updateRhs) f->updateRhs = rewriteIndexes(f->updateRhs, ctx);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (ie->cond) ie->cond = rewriteIndexes(ie->cond, ctx);
      break;  // branches handled by recurseNested
    }
    case StmtIRKind::ExprStmt:
      if (auto* e = static_cast<ExprStmtIR*>(stmt)->expr)
        static_cast<ExprStmtIR*>(stmt)->expr = rewriteIndexes(e, ctx);
      break;
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (a->targetExpr) a->targetExpr = rewriteIndexes(a->targetExpr, ctx);
      if (a->value) a->value = rewriteIndexes(a->value, ctx);
      break;
    }
    case StmtIRKind::VarDecl:
      if (auto* i = static_cast<VarDeclIR*>(stmt)->init)
        static_cast<VarDeclIR*>(stmt)->init = rewriteIndexes(i, ctx);
      break;
    case StmtIRKind::Return:
      if (auto* r = static_cast<ReturnIR*>(stmt)->value)
        static_cast<ReturnIR*>(stmt)->value = rewriteIndexes(r, ctx);
      break;
  }
}

bool isConst(DAGNode* n) {
  return n && n->kind == NodeKind::Constant && n->symbol.empty();
}

// If `stmt` advances a single known counter in place, return the `++x`/`--x`
// node; else nullptr. The two shapes are a bare expression statement (`++i;`)
// and a declaration initializer (`double z = ++i;`). Anything more involved
// (an increment nested in a larger expression) is handled by the write scan,
// which invalidates conservatively.
DAGNode* incDecTarget(StmtIR* stmt) {
  DAGNode* e = nullptr;
  if (stmt->kind == StmtIRKind::ExprStmt)
    e = static_cast<ExprStmtIR*>(stmt)->expr;
  else if (stmt->kind == StmtIRKind::VarDecl)
    e = static_cast<VarDeclIR*>(stmt)->init;
  if (e && isIncDec(e) && !e->operands.empty() &&
      e->operands[0]->kind == NodeKind::Variable)
    return e;
  return nullptr;
}

// Fold one statement's effect into the map of known constant counters.
//
// Every counter the statement may write must be invalidated -- a stale value
// lets a later `tensor[i]` be specialized to the wrong index. The invalidation
// has to see *all* of the statement's writes: a declaration initializer
// (`double z = ++i;` writes `i`), an assignment value, and anything reached
// through a reference (`r = 2` writes `r`'s referent). The previous version had
// fast paths that returned early for a VarDecl or a simple Assign without
// running the write scan, so those writes went unnoticed.
void updateKnown(StmtIR* stmt, ConstMap& known, const AliasInfo& aliases) {
  if (!stmt) return;

  // A bare `++x` / `--x` advances a known counter rather than invalidating it.
  // It still invalidates whatever `x` aliases (a reference used as a counter),
  // which is why the alias closure runs here too.
  if (DAGNode* inc = incDecTarget(stmt)) {
    const std::string& nm = inc->operands[0]->name;
    LocationSet w = aliases.writtenBy(stmt);
    if (w.anything()) {
      known.clear();
    } else {
      for (const auto& l : w.items())
        if (l.root != nm) known.erase(l.root);
    }
    auto it = known.find(nm);
    if (it != known.end()) it->second += (inc->name == "++") ? 1.0 : -1.0;
  } else {
    LocationSet w = aliases.writtenBy(stmt);
    if (w.anything()) {
      known.clear();
    } else {
      for (const auto& l : w.items()) known.erase(l.root);
    }
  }

  // Re-establish the counter a declaration / assignment defines.
  if (stmt->kind == StmtIRKind::VarDecl) {
    auto* d = static_cast<VarDeclIR*>(stmt);
    if (isConst(d->init)) known[d->name] = d->init->constVal;
    else known.erase(d->name);
  } else if (stmt->kind == StmtIRKind::Assign) {
    auto* a = static_cast<AssignIR*>(stmt);
    // An element/member write (`a[i] = v`) writes through memory and defines no
    // counter; its target was already invalidated above.
    if (!a->targetExpr) {
      if (isConst(a->value)) known[a->target] = a->value->constVal;
      else known.erase(a->target);
    }
  }
}

void processBlock(BlockIR* block, const Ctx& outer, const ConstMap& seed);
void processScope(StmtIR* s, const Ctx& outer, const ConstMap& seed);
void recurseNested(StmtIR* s, const Ctx& ctx, const ConstMap& known);

// The counters a nested scope starts from: those known at its entry, minus the
// names the scope may write. A `for` body is seeded this way -- its induction
// variable and any counter it increments are not constant across iterations, so
// they must not fold inside it. A bare block or an `if` branch keeps `known`
// unchanged (a counter known before it is still known at entry, and its writes
// do not propagate back out).
ConstMap entrySeedFor(StmtIR* scopeStmt, const ConstMap& known,
                      const AliasInfo& aliases) {
  ConstMap seed = known;
  LocationSet w = aliases.writtenBy(scopeStmt);
  if (w.anything()) {
    seed.clear();
  } else {
    for (const auto& l : w.items()) seed.erase(l.root);
  }
  return seed;
}

// Recurse into the nested scopes a statement introduces, each with a seed valid
// at its entry. Called before the statement's own writes are folded into the
// enclosing `known`, so the seed is the entry state.
void recurseNested(StmtIR* s, const Ctx& ctx, const ConstMap& known) {
  if (!s) return;
  switch (s->kind) {
    case StmtIRKind::Block:
      processBlock(static_cast<BlockIR*>(s), ctx, known);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(s);
      processScope(f->body.get(), ctx, entrySeedFor(s, known, ctx.aliases));
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(s);
      processScope(ie->thenBranch.get(), ctx, known);
      processScope(ie->elseBranch.get(), ctx, known);
      break;
    }
    // These kinds introduce no nested scope. Listed explicitly so a new
    // StmtIRKind is a compile error here rather than a silent skip.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
}

// Process a single-statement scope: a loop body or `if` branch that is not a
// Block. Rewrites its slots, recurses its own nested scopes, and folds its
// counter update into a local map (nothing propagates back out).
void processScope(StmtIR* s, const Ctx& outer, const ConstMap& seed) {
  if (!s) return;
  if (s->kind == StmtIRKind::Block) {
    processBlock(static_cast<BlockIR*>(s), outer, seed);
    return;
  }
  ConstMap known = seed;
  Ctx ctx{outer.mod, known, outer.names, outer.vecLocal, outer.aliases};
  rewriteStmt(s, ctx);
  recurseNested(s, ctx, known);
  updateKnown(s, known, ctx.aliases);
}

// Process a block in order: rewrite indices, fold constant `if`s, and update
// the map of known constant counters. Splices folded branches into the block.
// `seed` is the counters known at block entry (empty at function scope); the
// block owns a copy, so nothing it learns leaks back to the caller.
void processBlock(BlockIR* block, const Ctx& outer, const ConstMap& seed) {
  ConstMap known = seed;
  Ctx ctx{outer.mod, known, outer.names, outer.vecLocal, outer.aliases};
  std::vector<std::unique_ptr<StmtIR>> out;
  out.reserve(block->stmts.size());

  for (auto& stmtPtr : block->stmts) {
    StmtIR* stmt = stmtPtr.get();

    // Fold `if (<const>) ... else ...` into the taken branch, dropping the
    // `if` entirely (later passes / the verifier treat `if` as opaque).
    if (stmt->kind == StmtIRKind::IfElse) {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (!ie->isConstexpr && isConst(ie->cond)) {
        bool takeThen = ie->cond->constVal != 0;
        std::unique_ptr<StmtIR> branch =
            takeThen ? std::move(ie->thenBranch) : std::move(ie->elseBranch);
        if (branch) {
          // The spliced statements run unconditionally now, so their counter
          // updates must be folded into `known` exactly like a top-level one.
          auto splice = [&](std::unique_ptr<StmtIR> s) {
            rewriteStmt(s.get(), ctx);
            recurseNested(s.get(), ctx, known);
            updateKnown(s.get(), known, ctx.aliases);
            out.push_back(std::move(s));
          };
          if (branch->kind == StmtIRKind::Block) {
            auto* bb = static_cast<BlockIR*>(branch.get());
            for (auto& s : bb->stmts) splice(std::move(s));
          } else {
            splice(std::move(branch));
          }
        }
        continue;
      }
    }

    rewriteStmt(stmt, ctx);
    // Recurse into the statement's nested scopes with the counters known here,
    // before its own writes are folded in (the copy-in seed).
    recurseNested(stmt, ctx, known);
    updateKnown(stmt, known, ctx.aliases);
    out.push_back(std::move(stmtPtr));
  }

  block->stmts = std::move(out);
}

}  // namespace

void CounterPropPass::run(IRModule& module) {
  if (!module.body || module.body->kind != StmtIRKind::Block) return;

  // Collect all variable names so lowered vector locals can be resolved.
  std::unordered_set<std::string> names;
  collectMentionedNames(module.body.get(), names);
  ConstMap known;  // function scope: no counters known yet
  AliasInfo aliases = AliasInfo::build(module.body.get());
  Ctx ctx{module, known, names, _vectorLocalName, aliases};
  processBlock(static_cast<BlockIR*>(module.body.get()), ctx, known);
}

}  // namespace cse
