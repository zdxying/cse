// IR invariant gate.
//
// Runs the full pass pipeline over every fixture region with the invariant
// checks turned on, and asserts they hold:
//
//   * IRModule::verify() -- every owned node's hash still describes it, no
//     dangling operand, no two interned nodes denote the same node (D1);
//   * effect preservation -- no pass dropped or duplicated an observable effect
//     (D2).
//
// verify() is compiled out under NDEBUG, so this gate is meaningful only in a
// build with assertions enabled -- which is the default here (the test build
// uses -O2 without -DNDEBUG). `make sanitize` runs it under ASan/UBSan, so the
// checks see every pass on every fixture there.
//
// Usage: verify_ir_invariants <fixture.cpp> [more fixtures...]
//   Exit 0 iff every region passes with no invariant violation.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_builder.h"
#include "ir/stmt_walk.h"
#include "ir/ir_module.h"
#include "passes/pass_manager.h"
#include "sweep.h"

using namespace cse;

namespace {

// ---- Declaration visibility ----
//
// verify() checks node identity and effects, but it has no name model: a
// Variable is just a string, and a pass that erases a VarDecl leaves a
// reference that verify() cannot see. LoopUnroll's local inlining did exactly
// that (a chain longer than its four-round cap emitted `... + a8;` with no
// declaration). This walk is the missing half: every read of a name that is
// declared *somewhere* in the body must be visible at that point.
//
// A name that is never declared locally (a global, a parameter of the enclosing
// translation unit, the break/continue sentinels) is not checked: the IR has no
// module-level symbol table.

void noteReads(const DAGNode* e, std::vector<std::string>& out) {
  if (!e) return;
  if (e->kind == NodeKind::Variable && !e->name.empty()) out.push_back(e->name);
  for (const auto* op : e->operands) noteReads(op, out);
}

void collectDeclared(const StmtIR* s, std::unordered_set<std::string>& out) {
  if (!s) return;
  switch (s->kind) {
    case StmtIRKind::Block:
      for (const auto& sub : static_cast<const BlockIR*>(s)->stmts)
        collectDeclared(sub.get(), out);
      break;
    case StmtIRKind::ForLoop: {
      const auto* f = static_cast<const ForLoopIR*>(s);
      collectDeclared(f->init.get(), out);
      collectDeclared(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      const auto* ie = static_cast<const IfElseIR*>(s);
      collectDeclared(ie->thenBranch.get(), out);
      collectDeclared(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::VarDecl:
      if (!static_cast<const VarDeclIR*>(s)->name.empty())
        out.insert(static_cast<const VarDeclIR*>(s)->name);
      break;
    default:
      break;
  }
}

bool visible(const std::vector<std::unordered_set<std::string>>& scopes,
             const std::string& name) {
  for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
    if (it->count(name)) return true;
  return false;
}

bool checkExprScope(DAGNode* e, const std::vector<std::unordered_set<std::string>>& scopes,
                    const std::unordered_set<std::string>& declared,
                    std::string& msg) {
  std::vector<std::string> reads;
  noteReads(e, reads);
  for (const auto& r : reads) {
    if (declared.count(r) && !visible(scopes, r)) {
      msg = "reference to '" + r + "' with no visible declaration";
      return false;
    }
  }
  return true;
}

bool checkStmtScope(StmtIR* s,
                    std::vector<std::unordered_set<std::string>>& scopes,
                    const std::unordered_set<std::string>& declared,
                    std::string& msg) {
  if (!s) return true;
  switch (s->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(s);
      scopes.emplace_back();
      for (auto& sub : b->stmts) {
        if (!checkStmtScope(sub.get(), scopes, declared, msg)) return false;
      }
      scopes.pop_back();
      return true;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(s);
      scopes.emplace_back();
      if (!checkStmtScope(f->init.get(), scopes, declared, msg)) return false;
      if (!checkExprScope(f->cond, scopes, declared, msg)) return false;
      if (!checkExprScope(f->update, scopes, declared, msg)) return false;
      if (!checkExprScope(f->updateRhs, scopes, declared, msg)) return false;
      if (!checkStmtScope(f->body.get(), scopes, declared, msg)) return false;
      scopes.pop_back();
      return true;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(s);
      if (!checkExprScope(ie->cond, scopes, declared, msg)) return false;
      if (!checkStmtScope(ie->thenBranch.get(), scopes, declared, msg)) return false;
      if (!checkStmtScope(ie->elseBranch.get(), scopes, declared, msg)) return false;
      return true;
    }
    case StmtIRKind::ExprStmt:
      return checkExprScope(static_cast<ExprStmtIR*>(s)->expr, scopes, declared, msg);
    case StmtIRKind::Return:
      return checkExprScope(static_cast<ReturnIR*>(s)->value, scopes, declared, msg);
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(s);
      // The initializer sees the outer binding, then the name comes into scope.
      if (!checkExprScope(d->init, scopes, declared, msg)) return false;
      if (!d->name.empty()) scopes.back().insert(d->name);
      return true;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(s);
      if (!checkExprScope(a->targetExpr, scopes, declared, msg)) return false;
      if (!checkExprScope(a->value, scopes, declared, msg)) return false;
      if (!a->target.empty() && declared.count(a->target) &&
          !visible(scopes, a->target)) {
        msg = "store to '" + a->target + "' with no visible declaration";
        return false;
      }
      return true;
    }
  }
  return true;
}

// ---- Declared numeric kind ----
//
// `numKind` is derived metadata (dag_node.h) that the builder attaches when it
// sees a declaration, and several rewrites refuse to fire without it. Nothing
// held a *renamed* declaration to that rule: LoopUnroll renames a mutable
// body-local per iteration (`v` -> `v__u0`) and the fresh name kept
// `Unknown`, so `v / v` folded to `1.0` outside a loop and stayed a division
// inside one. Values are identical, so no numeric oracle saw it.
//
// The check: every locally declared name that a Variable node reads must carry
// the kind its declaration implies. A name that is not declared locally (a
// global, a parameter, a break/continue sentinel) is not checked -- the IR has
// no module-level symbol table, same caveat as the visibility walk above.

void collectDeclKinds(const StmtIR* s,
                      std::unordered_map<std::string, NumericKind>& out) {
  if (!s) return;
  switch (s->kind) {
    case StmtIRKind::Block:
      for (const auto& sub : static_cast<const BlockIR*>(s)->stmts)
        collectDeclKinds(sub.get(), out);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<const ForLoopIR*>(s);
      collectDeclKinds(f->init.get(), out);
      collectDeclKinds(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<const IfElseIR*>(s);
      collectDeclKinds(ie->thenBranch.get(), out);
      collectDeclKinds(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<const VarDeclIR*>(s);
      if (!d->name.empty()) out[d->name] = numericKindOfType(d->type);
      break;
    }
    // These kinds declare nothing.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::Return:
      break;
  }
}

bool checkExprKinds(const DAGNode* e,
                    const std::unordered_map<std::string, NumericKind>& decls,
                    std::string& msg) {
  if (!e) return true;
  if (e->kind == NodeKind::Variable && !e->name.empty()) {
    auto it = decls.find(e->name);
    if (it != decls.end() && e->numKind != it->second) {
      // Unknown is reported too: a declared `double` local whose node says
      // Unknown is exactly the lost-kind defect. (Unknown is legitimately the
      // declared kind for a pointer/template type, and then it matches.)
      msg = "variable '" + e->name + "' reads with numeric kind " +
            std::to_string(static_cast<int>(e->numKind)) +
            " but its declaration implies " +
            std::to_string(static_cast<int>(it->second)) +
            " (a renamed/copied declaration lost its kind)";
      return false;
    }
  }
  for (const auto* op : e->operands)
    if (!checkExprKinds(op, decls, msg)) return false;
  return true;
}

bool checkStmtKinds(
    const StmtIR* s, const std::unordered_map<std::string, NumericKind>& decls,
    std::string& msg) {
  if (!s) return true;
  bool ok = true;
  forEachExpr(const_cast<StmtIR*>(s), [&](DAGNode*& e) {
    if (ok && !checkExprKinds(e, decls, msg)) ok = false;
  });
  if (!ok) return false;
  switch (s->kind) {
    case StmtIRKind::Block:
      for (const auto& sub : static_cast<const BlockIR*>(s)->stmts)
        if (!checkStmtKinds(sub.get(), decls, msg)) return false;
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<const ForLoopIR*>(s);
      if (!checkStmtKinds(f->init.get(), decls, msg)) return false;
      if (!checkStmtKinds(f->body.get(), decls, msg)) return false;
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<const IfElseIR*>(s);
      if (!checkStmtKinds(ie->thenBranch.get(), decls, msg)) return false;
      if (!checkStmtKinds(ie->elseBranch.get(), decls, msg)) return false;
      break;
    }
    // These kinds own no nested statements.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
  return true;
}

// `declared` is collected from the freshly built IR, before any pass runs: a
// name that was a local then must still be visible afterwards. Recomputing it
// after the pipeline would quietly reclassify an erased local as a global.
bool checkScopes(IRModule& mod,
                 const std::unordered_set<std::string>& declared,
                 std::string& msg) {
  std::vector<std::unordered_set<std::string>> scopes;
  scopes.emplace_back();
  for (const auto& p : mod.funcSig.params) scopes.back().insert(p.name);
  return checkStmtScope(mod.body.get(), scopes, declared, msg);
}

}  // namespace

namespace {

Result checkRegion(const std::string& src, const CSEConfig& cfg,
                   std::string& msg) {
  std::vector<Token> tokens;
  Parser::ParseResult res;
  try {
    Lexer lexer(src, cfg);
    tokens = lexer.tokenize();
    Parser parser(tokens, cfg);
    res = parser.parseAll();
  } catch (const std::exception& e) {
    msg = std::string("frontend rejected the region: ") + e.what();
    return Result::Skip;
  }
  if (res.functions.empty()) {
    msg = "no function to check";
    return Result::Skip;
  }

  for (const auto& f : res.functions) {
    IRModule mod;
    IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);

    // The interning invariant must hold on the freshly built IR too.
    mod.verify();

    // Remember every local name while the declarations still exist.
    std::unordered_set<std::string> declared;
    collectDeclared(mod.body.get(), declared);
    for (const auto& p : mod.funcSig.params) declared.insert(p.name);

    auto pm = PassManager::createDefault(cfg, false, nullptr, nullptr);
    pm.setCheckEffects(true);
    pm.runAll(mod, false);  // calls verify() after every pass

    // ... and again once the whole pipeline has run.
    mod.verify();

    // No pass may leave a reference to a declaration it erased.
    std::string scopeMsg;
    if (!checkScopes(mod, declared, scopeMsg)) {
      msg = "function '" + f->name + "': " + scopeMsg;
      return Result::Fail;
    }

    // ... and no pass (or rename) may leave a declared variable reading with a
    // numeric kind its declaration contradicts.
    std::unordered_map<std::string, NumericKind> declKinds;
    collectDeclKinds(mod.body.get(), declKinds);
    std::string kindMsg;
    if (!checkStmtKinds(mod.body.get(), declKinds, kindMsg)) {
      msg = "function '" + f->name + "': " + kindMsg;
      return Result::Fail;
    }

    if (!pm.effectViolations().empty()) {
      msg = "function '" + f->name + "':\n";
      for (const auto& v : pm.effectViolations()) msg += v;
      return Result::Fail;
    }
  }
  return Result::Pass;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture.cpp> [more...]\n", argv[0]);
    return 2;
  }
  const int rc = sweepRegions(
      argc, argv, "ir-invariants", "regions",
      /*countAllRegions=*/false, /*printSkips=*/false,
      [](const std::string& code, std::string& msg) {
        return checkRegion(code, aggressive(), msg);
      });
  if (rc == 0) std::printf("IR-INVARIANTS OK\n");
  return rc;
}
