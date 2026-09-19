#include "loop_unroll.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"

namespace cse {

namespace {

// Does the expression contain an impure (side-effecting) call?
bool hasImpure(DAGNode* e) {
  if (!e) return false;
  if (e->kind == NodeKind::Call && !e->pure) return true;
  for (auto* op : e->operands)
    if (hasImpure(op)) return true;
  return false;
}

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
      out->value = a->value;
      return out;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<const VarDeclIR*>(stmt);
      auto out = std::make_unique<VarDeclIR>();
      out->type = d->type;
      out->name = d->name;
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
void substituteStmt(IRModule& mod, StmtIR* stmt, const std::string& name,
                    DAGNode* replacement) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) substituteStmt(mod, s.get(), name, replacement);
      break;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (a->value) a->value = substitute(mod, a->value, name, replacement);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(stmt);
      if (d->init) d->init = substitute(mod, d->init, name, replacement);
      break;
    }
    case StmtIRKind::ExprStmt: {
      auto* e = static_cast<ExprStmtIR*>(stmt);
      if (e->expr) e->expr = substitute(mod, e->expr, name, replacement);
      break;
    }
    case StmtIRKind::Return: {
      auto* r = static_cast<ReturnIR*>(stmt);
      if (r->value) r->value = substitute(mod, r->value, name, replacement);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (ie->cond) ie->cond = substitute(mod, ie->cond, name, replacement);
      substituteStmt(mod, ie->thenBranch.get(), name, replacement);
      substituteStmt(mod, ie->elseBranch.get(), name, replacement);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      substituteStmt(mod, f->init.get(), name, replacement);
      if (f->cond) f->cond = substitute(mod, f->cond, name, replacement);
      if (f->update) f->update = substitute(mod, f->update, name, replacement);
      if (f->updateRhs)
        f->updateRhs = substitute(mod, f->updateRhs, name, replacement);
      substituteStmt(mod, f->body.get(), name, replacement);
      break;
    }
  }
}

// Does the statement tree assign to `name`?
bool reassignedIn(StmtIR* stmt, const std::string& name) {
  if (!stmt) return false;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts)
        if (reassignedIn(s.get(), name)) return true;
      return false;
    }
    case StmtIRKind::Assign:
      return static_cast<AssignIR*>(stmt)->target == name;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      return reassignedIn(f->init.get(), name) || reassignedIn(f->body.get(), name);
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      return reassignedIn(ie->thenBranch.get(), name) ||
             reassignedIn(ie->elseBranch.get(), name);
    }
    default:
      return false;
  }
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
    default:
      break;
  }
}

// Inline confined local declarations (name -> init) and drop the declarations.
// `inlinable` holds names whose uses are fully contained in the loop body.
void inlineLocalDecls(IRModule& mod, StmtIR* stmt,
                      const std::set<std::string>& inlinable) {
  if (!stmt || stmt->kind != StmtIRKind::Block) return;
  auto* b = static_cast<BlockIR*>(stmt);

  std::unordered_map<std::string, DAGNode*> defs;
  auto it = b->stmts.begin();
  while (it != b->stmts.end()) {
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* d = static_cast<VarDeclIR*>(it->get());
      if (d->init && d->init->kind != NodeKind::Variable &&
          !hasImpure(d->init) && inlinable.count(d->name)) {
        defs[d->name] = d->init;
        it = b->stmts.erase(it);
        continue;
      }
    }
    ++it;
  }
  if (defs.empty()) return;

  for (int iter = 0; iter < 4 && !defs.empty(); ++iter) {
    for (auto& [name, val] : defs) {
      substituteStmt(mod, b, name, val);
    }
  }
}

// Match `for (i = 0; i < N; ++i)` / `for (i = 0; i < N; i += 1)`.
bool matchCountedLoop(ForLoopIR* f, int maxUnroll, std::string& var,
                      int& count) {
  if (!f->init || f->init->kind != StmtIRKind::VarDecl) return false;
  auto* d = static_cast<VarDeclIR*>(f->init.get());
  if (d->name.empty()) return false;
  if (d->init && !(d->init->kind == NodeKind::Constant && d->init->constVal == 0))
    return false;

  if (!f->cond || f->cond->kind != NodeKind::BinaryOp || f->cond->op != '<')
    return false;
  if (f->cond->operands.size() != 2) return false;
  DAGNode* lhs = f->cond->operands[0];
  DAGNode* rhs = f->cond->operands[1];
  if (!(lhs->kind == NodeKind::Variable && lhs->name == d->name)) return false;
  if (rhs->kind != NodeKind::Constant) return false;
  int n = static_cast<int>(rhs->constVal);
  if (n <= 0 || n > maxUnroll) return false;

  // update: ++var, var++, or var = var + 1
  bool updateOk = false;
  if (f->update && f->update->kind == NodeKind::UnaryOp && f->update->name == "++" &&
      f->update->op == '+' && !f->update->operands.empty() &&
      f->update->operands[0]->kind == NodeKind::Variable &&
      f->update->operands[0]->name == d->name) {
    updateOk = true;
  } else if (f->update && f->update->kind == NodeKind::Variable &&
             f->update->name == d->name && f->updateOp == '+') {
    updateOk = !f->updateRhs ||
               (f->updateRhs->kind == NodeKind::Constant &&
                f->updateRhs->constVal == 1);
  }
  if (!updateOk) return false;

  if (reassignedIn(f->body.get(), d->name)) return false;

  var = d->name;
  count = n;
  return true;
}

std::unique_ptr<StmtIR> unrollStmt(IRModule& mod, std::unique_ptr<StmtIR> stmt,
                                   int maxUnroll,
                                   const std::unordered_map<std::string, int>& globalUses) {
  if (!stmt) return stmt;

  if (stmt->kind == StmtIRKind::Block) {
    auto* b = static_cast<BlockIR*>(stmt.get());
    std::vector<std::unique_ptr<StmtIR>> flat;
    for (auto& s : b->stmts) {
      auto u = unrollStmt(mod, std::move(s), maxUnroll, globalUses);
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
    f->init = unrollStmt(mod, std::move(f->init), maxUnroll, globalUses);
    f->body = unrollStmt(mod, std::move(f->body), maxUnroll, globalUses);

    std::string var;
    int count = 0;
    if (!matchCountedLoop(f, maxUnroll, var, count)) return stmt;

    // Only unroll if every local declared in the body is confined to the body;
    // otherwise inlining the declarations would change semantics.
    std::set<std::string> decls;
    collectDeclNames(f->body.get(), decls);
    auto bodyUses = countUses(f->body.get());
    std::set<std::string> inlinable;
    for (auto& name : decls) {
      auto g = globalUses.find(name);
      auto b = bodyUses.find(name);
      int gv = (g == globalUses.end()) ? 0 : g->second;
      int bv = (b == bodyUses.end()) ? 0 : b->second;
      if (gv == bv) inlinable.insert(name);
    }
    for (auto& name : decls) {
      if (!inlinable.count(name)) return stmt;  // not safe to unroll
    }

    auto block = std::make_unique<BlockIR>();
    for (int i = 0; i < count; ++i) {
      auto clone = cloneStmt(f->body.get());
      DAGNode* c = mod.createConst(i, std::to_string(i));
      substituteStmt(mod, clone.get(), var, c);
      inlineLocalDecls(mod, clone.get(), inlinable);
      block->stmts.push_back(std::move(clone));
    }
    return block;
  }

  return stmt;
}

}  // namespace

void LoopUnrollPass::run(IRModule& module) {
  if (!module.body) return;
  auto globalUses = countUses(module.body.get());
  module.body =
      unrollStmt(module, std::move(module.body), _maxUnroll, globalUses);
}

}  // namespace cse
