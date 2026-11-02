#include "loop_unroll.h"

#include <climits>
#include <cmath>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../analysis/effects.h"
#include "../analysis/purity.h"
#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/location.h"
#include "../ir/stmt_walk.h"
#include "../ir/statement.h"

namespace cse {

namespace {

// Deep-copy a statement tree. DAGNode* pointers are shared (the module owns
// them), so cloning only duplicates the statement structure, not the DAG.
std::unique_ptr<StmtIR> cloneStmt(const StmtIR* stmt) {
  if (!stmt) return nullptr;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<const BlockIR*>(stmt);
      auto out = std::make_unique<BlockIR>();
      for (auto& s : b->stmts) out->stmts.push_back(cloneStmt(s.get()));
      return out;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<const AssignIR*>(stmt);
      auto out = std::make_unique<AssignIR>();
      out->target = a->target;
      out->targetExpr = a->targetExpr;
      out->value = a->value;
      // A compound store (`a[i] += x`) must keep its operator: dropping it turns
      // the store into a plain `a[i] = x`, silently discarding the read of the
      // old value. This is the one field a clone cannot afford to lose.
      out->compoundOp = a->compoundOp;
      return out;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<const VarDeclIR*>(stmt);
      auto out = std::make_unique<VarDeclIR>();
      out->type = d->type;
      out->name = d->name;
      // The array-declarator bounds are part of the declaration: dropping them
      // turns `double arr[7]` into `double [7] arr` (the codegen splits the
      // bounds out of the type text only when arrayBounds is present), which is
      // where a cloned in-loop array declaration failed to compile.
      out->arrayBounds = d->arrayBounds;
      out->init = d->init;
      return out;
    }
    case StmtIRKind::ExprStmt: {
      auto* e = static_cast<const ExprStmtIR*>(stmt);
      auto out = std::make_unique<ExprStmtIR>();
      out->expr = e->expr;
      return out;
    }
    case StmtIRKind::Return: {
      auto* r = static_cast<const ReturnIR*>(stmt);
      auto out = std::make_unique<ReturnIR>();
      out->value = r->value;
      return out;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<const IfElseIR*>(stmt);
      auto out = std::make_unique<IfElseIR>();
      out->cond = ie->cond;
      out->isConstexpr = ie->isConstexpr;
      out->thenBranch = cloneStmt(ie->thenBranch.get());
      out->elseBranch = cloneStmt(ie->elseBranch.get());
      return out;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<const ForLoopIR*>(stmt);
      auto out = std::make_unique<ForLoopIR>();
      out->init = cloneStmt(f->init.get());
      out->cond = f->cond;
      out->update = f->update;
      out->updateRhs = f->updateRhs;
      out->updateOp = f->updateOp;
      out->body = cloneStmt(f->body.get());
      return out;
    }
  }
  return nullptr;
}

// Replace every Variable node named `name` inside a statement tree.
//
// The expression slots -- an element store contributes its lvalue as well as its
// value -- come from stmt_walk.h, so a new slot cannot be missed here.
bool substituteStmt(IRModule& mod, StmtIR* stmt, const std::string& name,
                    DAGNode* replacement) {
  bool changed = false;
  forEachExprDeep(stmt, [&](DAGNode*& e) {
    DAGNode* r = substitute(mod, e, name, replacement);
    if (r != e) {
      e = r;
      changed = true;
    }
  });
  return changed;
}

// Clone-with-freshening: rebuilt non-pure nodes (memory loads, impure calls)
// must be distinct per unrolled iteration, otherwise they would be shared
// across iterations and CSE would wrongly treat them as invariant.
DAGNode* freshen(IRModule& mod, DAGNode* n) {
  if (!n) return n;
  bool changed = false;
  std::vector<DAGNode*> ops;
  ops.reserve(n->operands.size());
  for (auto* op : n->operands) {
    DAGNode* r = freshen(mod, op);
    ops.push_back(r);
    if (r != op) changed = true;
  }
  if (n->pure && !changed) return n;
  // Impure nodes are never interned, so rebuilding them -- even with unchanged
  // operands -- is what gives each unrolled iteration its own load / call.
  return rebuildWithOperands(mod, n, ops);
}

// Give every expression in the statement tree its own impure loads / calls.
void freshenStmt(IRModule& mod, StmtIR* stmt) {
  forEachExprDeep(stmt, [&](DAGNode*& e) { e = freshen(mod, e); });
}

// Rename a local variable's declaration and assignment targets in a statement
// tree (uses are handled separately via substituteStmt).
//
// The renamed variable must inherit the original's numeric kind. `numKind` is
// derived metadata (dag_node.h) that the builder sets when it sees the
// declaration, so a fresh name it has never seen stays `Unknown` -- and a
// rewrite that needs the kind (folding `v / v` to `1`, dropping a floating
// `1.0` factor) then refuses to fire. Renaming a `double` local to `v__u0`
// used to leave that iteration's copy untyped while the original was `Float`,
// so the same source optimized differently depending on whether its loop was
// unrolled.
void renameLocal(IRModule& mod, StmtIR* stmt, const std::string& from,
                 const std::string& to) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) renameLocal(mod, s.get(), from, to);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      renameLocal(mod, f->init.get(), from, to);
      renameLocal(mod, f->body.get(), from, to);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      renameLocal(mod, ie->thenBranch.get(), from, to);
      renameLocal(mod, ie->elseBranch.get(), from, to);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(stmt);
      if (d->name == from) {
        d->name = to;
        // Carry the declared kind across the rename (see the function comment).
        mod.setVarKind(to, numericKindOfType(d->type));
      }
      break;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (!a->targetExpr && a->target == from) a->target = to;
      break;
    }
    // A rename touches declarations and assignment targets only; these kinds
    // own neither. Listed so a new kind is a compile error here.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Return:
      break;
  }
}

// Does the statement tree write to `name`?
//
// This has to see *every* kind of write, not just an `AssignIR` target. The
// kind-by-kind version it replaces listed only `AssignIR`, so a `++`/`--`
// (`i++` in the loop body, `t--` on a body-local) or an assignment spelled as
// an expression was invisible. The unroller then substituted a constant into an
// increment (`0.0++`) or inlined a local into its own `++` (`(a + 1.0)++`),
// neither of which compiles.
//
// `collectWriteLocations` (ir/location.h) owns the complete set of written
// locations --
// inc/dec, expression-assignments, structured store roots and a `for` update
// normalized out of the expression tree -- so this test cannot go stale again
// when a write shape is added.
bool reassignedIn(StmtIR* stmt, const std::string& name,
                  const AliasInfo& aliases) {
  if (!stmt) return false;
  return aliases.writtenBy(stmt).containsRoot(name);
}

// Does the statement tree contain a `break`/`continue` that belongs to the
// enclosing loop?
//
// A `break` or `continue` nested inside a *further* loop belongs to that loop,
// so the walk does not descend into a nested ForLoop's body.
//
// Unrolling splices each iteration's body into the parent block, which would
// move such a statement out of the loop it names: a top-level `break;` does not
// compile, and even where it did the control flow would be wrong.
bool containsLoopBreakOrContinue(StmtIR* stmt) {
  if (!stmt) return false;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts)
        if (containsLoopBreakOrContinue(s.get())) return true;
      return false;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      return containsLoopBreakOrContinue(ie->thenBranch.get()) ||
             containsLoopBreakOrContinue(ie->elseBranch.get());
    }
    case StmtIRKind::ExprStmt:
      // The only shape that can *be* a `break;`/`continue;`: the frontend has no
      // keyword for them, so they arrive as a bare-variable expression. The
      // encoding lives in one place (isLoopControl, ir/stmt_walk.h).
      return isLoopControl(stmt);
    case StmtIRKind::ForLoop:
      // A further nested loop owns its own break/continue; this walk must not
      // claim them for the loop being unrolled.
      return false;
    // These kinds have no slot that can hold a break or continue.
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      return false;
  }
  return false;
}

void collectDeclNames(StmtIR* stmt, std::set<std::string>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) collectDeclNames(s.get(), out);
      break;
    }
    case StmtIRKind::VarDecl:
      out.insert(static_cast<VarDeclIR*>(stmt)->name);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectDeclNames(f->init.get(), out);
      collectDeclNames(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectDeclNames(ie->thenBranch.get(), out);
      collectDeclNames(ie->elseBranch.get(), out);
      break;
    }
    // These kinds declare nothing.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::Return:
      break;
  }
}

// Record each declared name's initializer (last declaration wins; IRBuilder
// alpha-renames shadowed names so a name is unique per scope).
void collectDeclInits(StmtIR* stmt,
                      std::unordered_map<std::string, DAGNode*>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) collectDeclInits(s.get(), out);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(stmt);
      out[d->name] = d->init;
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectDeclInits(f->init.get(), out);
      collectDeclInits(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectDeclInits(ie->thenBranch.get(), out);
      collectDeclInits(ie->elseBranch.get(), out);
      break;
    }
    // These kinds declare nothing.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::Return:
      break;
  }
}

// Is every node in `e` referentially transparent? See
// `isReferentiallyTransparent` in analysis/purity.h -- a node carrying
// `pure == false` (a non-shareable load, an impure call, a `++`/`--`) denotes a
// different value at a different point, so it may neither be repeated nor moved.
//
// A body-local declaration can be unrolled only if its initializer can be
// inlined at every use and the variable is never reassigned.
//
// "Inlined at every use" means the initializer is evaluated at each use site,
// which may sit *after* a store the original declaration preceded. It must
// therefore be referentially transparent (`isReferentiallyTransparent`), and it
// must not itself write (`hasObservableEffect`, which also catches an assignment
// spelled as an expression -- that node has `pure == true`). The earlier test
// asked only "contains an impure call", which missed non-shareable loads
// entirely: `double t = a[i]; a[i] = 0; use(t);` was rewritten to read `a[i]`
// after the store.
bool declInlinable(DAGNode* init) {
  return init && init->kind != NodeKind::Variable &&
         !hasObservableEffect(init) && isReferentiallyTransparent(init);
}

// Does `init` read a variable that is written somewhere in the loop body?
//
// Purity is not enough. `double t = a + 1.0; a = a + 2.0; s += t;` has a pure
// initializer, but inlining it moves the read of `a` to the use site, *after*
// the write -- so the use sees the new `a`. `writeAffectsRead` resolves the read
// through reference edges (a read of `s3`, a reference to `b`, is changed by a
// write to `b`) and is wildcard-aware (`"*"`, the wildcard an impure call
// contributes, means a read across it is not stable either).
bool readsWrittenVar(DAGNode* init, const LocationSet& written,
                     const AliasInfo& aliases) {
  std::unordered_set<std::string> deps;
  collectReadNames(init, deps);
  if (deps.empty()) return false;
  for (const auto& d : deps)
    if (aliases.writeAffectsRead(written, d)) return true;
  return false;
}

// Inline confined local declarations (name -> init) and drop the declarations.
// `inlinable` holds names whose uses are fully contained in the loop body.
// Recurses into nested statements so declarations in nested blocks are removed
// too; IRBuilder alpha-renames shadowed names, so substituting globally within
// the body is safe.
void inlineLocalDecls(IRModule& mod, StmtIR* stmt,
                      const std::set<std::string>& inlinable) {
  if (!stmt) return;
  if (stmt->kind != StmtIRKind::Block) {
    switch (stmt->kind) {
      case StmtIRKind::ForLoop: {
        auto* f = static_cast<ForLoopIR*>(stmt);
        inlineLocalDecls(mod, f->init.get(), inlinable);
        inlineLocalDecls(mod, f->body.get(), inlinable);
        break;
      }
      case StmtIRKind::IfElse: {
        auto* ie = static_cast<IfElseIR*>(stmt);
        inlineLocalDecls(mod, ie->thenBranch.get(), inlinable);
        inlineLocalDecls(mod, ie->elseBranch.get(), inlinable);
        break;
      }
      // These kinds own no nested statements.
      case StmtIRKind::ExprStmt:
      case StmtIRKind::Assign:
      case StmtIRKind::VarDecl:
      case StmtIRKind::Return:
        break;
      case StmtIRKind::Block:
        // Unreachable: the early return above handles every Block. Listed so
        // the switch stays exhaustive over StmtIRKind.
        break;
    }
    return;
  }

  auto* b = static_cast<BlockIR*>(stmt);
  std::unordered_map<std::string, DAGNode*> defs;
  auto it = b->stmts.begin();
  while (it != b->stmts.end()) {
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* d = static_cast<VarDeclIR*>(it->get());
      if (declInlinable(d->init) && inlinable.count(d->name)) {
        defs[d->name] = d->init;
        it = b->stmts.erase(it);
        continue;
      }
    }
    ++it;
  }

  // Recurse into nested structured statements to drop their declarations too.
  for (auto& s : b->stmts) {
    if (s->kind == StmtIRKind::ForLoop || s->kind == StmtIRKind::IfElse) {
      inlineLocalDecls(mod, s.get(), inlinable);
    }
  }

  if (defs.empty()) return;

  // Substitute to a fixpoint. The old bound of four rounds was a hard cap, not
  // a convergence test: every declaration had already been erased, so a chain
  // longer than four links kept references to names that no longer existed and
  // the emitted code did not compile (`... + a8;` with no `a8`). Each round
  // also rewrites the stored definitions, so a chain resolves regardless of the
  // unordered_map's iteration order. The round bound is defs.size() + 1: a
  // well-formed chain cannot need more, and a cycle (which the frontend cannot
  // produce) terminates instead of hanging.
  for (size_t round = 0; round <= defs.size(); ++round) {
    bool changed = false;
    for (auto& [name, val] : defs) {
      if (substituteStmt(mod, b, name, val)) changed = true;
      for (auto& [otherName, otherVal] : defs) {
        if (otherName == name) continue;
        DAGNode* r = substitute(mod, otherVal, name, val);
        if (r != otherVal) {
          otherVal = r;
          changed = true;
        }
      }
    }
    if (!changed) break;
  }
}

// Match `for (i = 0; i < N; ++i)` / `for (i = 0; i < N; i += 1)`.
//
// `outsideUses`, when non-null, maps a name to how many times it is read
// *outside this loop statement* in the enclosing function. Unrolling deletes the
// loop and the counter it declares, so a read outside becomes a reference to a
// name that no longer exists (see the check at the end of this function).
bool matchCountedLoop(ForLoopIR* f, int maxUnroll, std::string& var,
                      int& start, int& count, const AliasInfo& aliases,
                      const std::unordered_map<std::string, int>* outsideUses) {
  if (!f->init || f->init->kind != StmtIRKind::VarDecl) return false;
  auto* d = static_cast<VarDeclIR*>(f->init.get());
  if (d->name.empty()) return false;
  if (d->init && d->init->kind != NodeKind::Constant) return false;
  // The start must be integral too: `for (double i = 0.5; i < 3; ++i)` is not
  // the same loop as `for (i = 0; ...)`, and truncating the start silently
  // changes the result.
  double beginVal = d->init ? d->init->constVal : 0.0;
  if (beginVal != std::floor(beginVal)) return false;
  int begin = static_cast<int>(beginVal);

  if (!f->cond || f->cond->kind != NodeKind::BinaryOp ||
      f->cond->op != OpCode::Lt)
    return false;
  if (f->cond->operands.size() != 2) return false;
  DAGNode* lhs = f->cond->operands[0];
  DAGNode* rhs = f->cond->operands[1];
  if (!(lhs->kind == NodeKind::Variable && lhs->name == d->name)) return false;
  if (rhs->kind != NodeKind::Constant) return false;
  // The bound must be exactly integral and fit an int: `i < 4.5` is a real loop
  // of five iterations, and truncating it to `int` (4) would silently drop the
  // last one; a bound above INT_MAX would overflow the cast.
  double bound = rhs->constVal;
  if (bound != std::floor(bound)) return false;
  if (bound < 0.0 || bound > static_cast<double>(INT_MAX)) return false;
  int n = static_cast<int>(bound);
  if (begin < 0 || n <= begin || (n - begin) > maxUnroll) return false;

  // update: prefix ++var, postfix var++, or var += 1. A postfix `var++` is
  // normalized by the builder to `update = var` with updateOp Inc; a compound
  // `var += 1` is `update = var` with updateOp Add and updateRhs 1.
  bool updateOk = false;
  if (isIncDec(f->update) && f->update->name == "++" &&
      f->update->op == OpCode::Inc && !f->update->operands.empty() &&
      f->update->operands[0]->kind == NodeKind::Variable &&
      f->update->operands[0]->name == d->name) {
    updateOk = true;
  } else if (f->update && f->update->kind == NodeKind::Variable &&
             f->update->name == d->name &&
             (f->updateOp == OpCode::Inc || f->updateOp == OpCode::Add)) {
    updateOk = !f->updateRhs ||
               (f->updateRhs->kind == NodeKind::Constant &&
                f->updateRhs->constVal == 1);
  }
  if (!updateOk) return false;

  // A `break`/`continue` in the body belongs to this loop; unrolling would
  // splice it out of the loop it names.
  if (containsLoopBreakOrContinue(f->body.get())) return false;

  if (reassignedIn(f->body.get(), d->name, aliases)) return false;

  // The counter must not be read outside the loop *statement*. Unrolling
  // splices the body into the parent block and drops the `for`, taking the
  // counter's declaration with it; a read of `i` after the loop then names
  // nothing. (Its own condition and update do not count -- they are part of the
  // statement being replaced.)
  //
  // This is not a miscompile of a valid program: `return s + i;` after a
  // `for (int i = ...)` does not compile in the first place. Refusing still
  // matters, because turning one compile error into a different one -- or, when
  // an outer `i` exists, silently rebinding to it -- is the wrong answer.
  if (outsideUses) {
    auto it = outsideUses->find(d->name);
    if (it != outsideUses->end() && it->second > 0) return false;
  }

  var = d->name;
  start = begin;
  count = n;
  return true;
}

// `modUses` is the read-count of the *whole function*, captured once before the
// walk. It is what "is this name read outside the loop?" is measured against:
// `countReadNames` on the loop statement alone cannot see the code that follows
// it. Nested loops pass the same map down -- their "outside" is the function,
// not the enclosing loop.
std::unique_ptr<StmtIR> unrollStmt(
    IRModule& mod, std::unique_ptr<StmtIR> stmt, int maxUnroll,
    const std::unordered_map<std::string, int>& modUses) {
  if (!stmt) return stmt;

  if (stmt->kind == StmtIRKind::Block) {
    auto* b = static_cast<BlockIR*>(stmt.get());
    std::vector<std::unique_ptr<StmtIR>> flat;
    for (auto& s : b->stmts) {
      auto u = unrollStmt(mod, std::move(s), maxUnroll, modUses);
      if (u && u->kind == StmtIRKind::Block) {
        // Splice unrolled loop bodies into the parent so cross-statement CSE
        // sees each iteration as a top-level statement.
        auto* inner = static_cast<BlockIR*>(u.get());
        for (auto& is : inner->stmts) flat.push_back(std::move(is));
      } else {
        flat.push_back(std::move(u));
      }
    }
    b->stmts = std::move(flat);
    return stmt;
  }

  if (stmt->kind == StmtIRKind::ForLoop) {
    auto* f = static_cast<ForLoopIR*>(stmt.get());
    f->init = unrollStmt(mod, std::move(f->init), maxUnroll, modUses);
    f->body = unrollStmt(mod, std::move(f->body), maxUnroll, modUses);

    // Rebuild the alias info *here*, on the already-inner-unrolled body: the
    // inner unroll introduced fresh names (`r__u0 = x__u0`), and an AliasInfo
    // captured before it would not know their reference edges. That stale map
    // missed the `r__u0 -> x__u0` edge, so `x__u0` looked never-reassigned and
    // its constant initializer was inlined into the reference binding
    // (`double& r__u0__u0 = 0.5;`).
    AliasInfo aliases = AliasInfo::build(f);

    std::string var;
    int start = 0, count = 0;

    // Use counts on the (already inner-unrolled) loop, so the map is current.
    // `globalUses` covers the loop statement itself; `bodyUses` its body.
    auto globalUses = countReadNames(stmt.get());
    auto bodyUses = countReadNames(f->body.get());

    // Reads of the counter outside this loop statement. `modUses` covers the
    // whole function; subtracting everything this loop statement reads (body,
    // condition, update) leaves the reads from the enclosing code. Those are the
    // ones that would dangle once the loop -- and the counter's declaration --
    // are gone.
    //
    // A body local gets the same treatment below via `gv != bv`, but there the
    // reference is `globalUses` (the loop statement) because a body local's
    // uses must be confined to the *body*. The counter's declaration lives in
    // the loop statement, so its "outside" is the whole function.
    std::unordered_map<std::string, int> outsideUses;
    {
      std::unordered_map<std::string, int> inside = bodyUses;
      countReadNames(f->cond, inside);
      countReadNames(f->update, inside);
      countReadNames(f->updateRhs, inside);
      for (const auto& kv : modUses) {
        auto it = inside.find(kv.first);
        const int inner = it == inside.end() ? 0 : it->second;
        const int outer = kv.second - inner;
        if (outer > 0) outsideUses[kv.first] = outer;
      }
    }

    if (!matchCountedLoop(f, maxUnroll, var, start, count, aliases,
                          &outsideUses))
      return stmt;

    // Every body-local must be confined to the body. Those that can be inlined
    // are removed; the rest are renamed per iteration so clones never share a
    // mutable variable (which later passes would treat as loop-invariant).
    std::set<std::string> decls;
    collectDeclNames(f->body.get(), decls);
    std::unordered_map<std::string, DAGNode*> declInits;
    collectDeclInits(f->body.get(), declInits);
    std::set<std::string> inlinable;
    std::set<std::string> renames;
    // Names written anywhere in the body, closed over reference edges. A
    // body-local whose initializer reads one of these cannot be inlined: the
    // read would move past the write.
    LocationSet bodyWritten = aliases.writtenBy(f->body.get());
    for (auto& name : decls) {
      auto g = globalUses.find(name);
      auto b = bodyUses.find(name);
      int gv = (g == globalUses.end()) ? 0 : g->second;
      int bv = (b == bodyUses.end()) ? 0 : b->second;
      if (gv != bv) return stmt;  // used outside the body: cannot unroll
      auto di = declInits.find(name);
      DAGNode* init = (di == declInits.end()) ? nullptr : di->second;
      // A reference's referent is an lvalue binding target, not a value: inlining
      // it would bind the reference to an rvalue (see isAliasedReferent). Rename
      // it with the other mutable locals instead.
      if (declInlinable(init) && !reassignedIn(f->body.get(), name, aliases) &&
          !aliases.isAliasedReferent(name) &&
          !readsWrittenVar(init, bodyWritten, aliases)) {
        inlinable.insert(name);
      } else {
        renames.insert(name);
      }
    }

    auto block = std::make_unique<BlockIR>();
    for (int i = start; i < count; ++i) {
      auto clone = cloneStmt(f->body.get());
      DAGNode* c = mod.createConst(i, std::to_string(i));
      substituteStmt(mod, clone.get(), var, c);
      // Give each iteration its own impure loads / calls.
      freshenStmt(mod, clone.get());
      // Rename mutable/impure body locals so iterations do not alias. The new
      // name comes from `freshName`: a user parameter spelled `t__u0` used to
      // collide with the first iteration's renamed local, producing a
      // declaration that shadows a parameter (or, worse, silently rebinding).
      for (auto& name : renames) {
        std::string to = mod.freshName(name + "__u" + std::to_string(i));
        renameLocal(mod, clone.get(), name, to);
        substituteStmt(mod, clone.get(), name, mod.getVar(to));
      }
      inlineLocalDecls(mod, clone.get(), inlinable);
      block->stmts.push_back(std::move(clone));
    }
    // Unroll loops newly exposed by substituting this loop's variable
    // (e.g. a triangular inner loop whose start depended on the outer var).
    // Recompute use counts: locals were renamed above, so the outer map is stale.
    return unrollStmt(mod, std::move(block), maxUnroll, modUses);
  }

  return stmt;
}

}  // namespace

void LoopUnrollPass::run(IRModule& module) {
  if (!module.body) return;
  const auto modUses = countReadNames(module.body.get());
  module.body =
      unrollStmt(module, std::move(module.body), _maxUnroll, modUses);
}

}  // namespace cse
