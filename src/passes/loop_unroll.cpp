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
      out->targetExpr = a->targetExpr;
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
      if (a->targetExpr) a->targetExpr = substitute(mod, a->targetExpr, name, replacement);
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
  switch (n->kind) {
    case NodeKind::Constant:
    case NodeKind::Variable:
      return n;
    case NodeKind::BinaryOp:
      if (ops.size() == 2) return mod.createBinaryOp(n->op, ops[0], ops[1]);
      break;
    case NodeKind::UnaryOp:
      if (ops.size() == 1) {
        DAGNode* u = mod.createUnaryOp(n->op, ops[0]);
        if (!n->name.empty()) u->name = n->name;
        return u;
      }
      break;
    case NodeKind::ArrayAccess:
      if (ops.size() == 2) return mod.createArrayAccess(ops[0], ops[1], n->pure);
      break;
    case NodeKind::MemberAccess:
      if (ops.size() == 1) return mod.createMemberAccess(ops[0], n->name, n->pure);
      break;
    case NodeKind::ArrowAccess:
      if (ops.size() == 1) return mod.createArrowAccess(ops[0], n->name, n->pure);
      break;
    case NodeKind::Call: {
      std::vector<DAGNode*> args(ops.begin() + 1, ops.end());
      return mod.createCall(ops[0], args, n->pure);
    }
    case NodeKind::Cast: {
      DAGNode* node = mod.createNode(NodeKind::Cast);
      node->name = n->name;
      node->operands = ops;
      return mod.findExistingNode(node);
    }
    case NodeKind::Ternary: {
      DAGNode* node = mod.createNode(NodeKind::Ternary);
      node->op = n->op;
      node->operands = ops;
      return mod.findExistingNode(node);
    }
    default:
      break;
  }
  return n;
}

void freshenStmt(IRModule& mod, StmtIR* stmt) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) freshenStmt(mod, s.get());
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      freshenStmt(mod, f->init.get());
      if (f->cond) f->cond = freshen(mod, f->cond);
      if (f->update) f->update = freshen(mod, f->update);
      if (f->updateRhs) f->updateRhs = freshen(mod, f->updateRhs);
      freshenStmt(mod, f->body.get());
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (ie->cond) ie->cond = freshen(mod, ie->cond);
      freshenStmt(mod, ie->thenBranch.get());
      freshenStmt(mod, ie->elseBranch.get());
      break;
    }
    case StmtIRKind::ExprStmt:
      if (auto* e = static_cast<ExprStmtIR*>(stmt)->expr)
        static_cast<ExprStmtIR*>(stmt)->expr = freshen(mod, e);
      break;
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (a->targetExpr) a->targetExpr = freshen(mod, a->targetExpr);
      if (a->value) a->value = freshen(mod, a->value);
      break;
    }
    case StmtIRKind::VarDecl:
      if (auto* i = static_cast<VarDeclIR*>(stmt)->init)
        static_cast<VarDeclIR*>(stmt)->init = freshen(mod, i);
      break;
    case StmtIRKind::Return:
      if (auto* r = static_cast<ReturnIR*>(stmt)->value)
        static_cast<ReturnIR*>(stmt)->value = freshen(mod, r);
      break;
  }
}

// Rename a local variable's declaration and assignment targets in a statement
// tree (uses are handled separately via substituteStmt).
void renameLocal(StmtIR* stmt, const std::string& from, const std::string& to) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) renameLocal(s.get(), from, to);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      renameLocal(f->init.get(), from, to);
      renameLocal(f->body.get(), from, to);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      renameLocal(ie->thenBranch.get(), from, to);
      renameLocal(ie->elseBranch.get(), from, to);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(stmt);
      if (d->name == from) d->name = to;
      break;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (!a->targetExpr && a->target == from) a->target = to;
      break;
    }
    default:
      break;
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
    default:
      break;
  }
}

// A body-local declaration can be unrolled only if its initializer can be
// inlined at every use and the variable is never reassigned.
bool declInlinable(DAGNode* init) {
  return init && init->kind != NodeKind::Variable && !hasImpure(init);
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
      default:
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

  for (int iter = 0; iter < 4 && !defs.empty(); ++iter) {
    for (auto& [name, val] : defs) {
      substituteStmt(mod, b, name, val);
    }
  }
}

// Match `for (i = 0; i < N; ++i)` / `for (i = 0; i < N; i += 1)`.
bool matchCountedLoop(ForLoopIR* f, int maxUnroll, std::string& var,
                      int& start, int& count) {
  if (!f->init || f->init->kind != StmtIRKind::VarDecl) return false;
  auto* d = static_cast<VarDeclIR*>(f->init.get());
  if (d->name.empty()) return false;
  if (d->init && d->init->kind != NodeKind::Constant) return false;
  int begin = d->init ? static_cast<int>(d->init->constVal) : 0;

  if (!f->cond || f->cond->kind != NodeKind::BinaryOp || f->cond->op != '<')
    return false;
  if (f->cond->operands.size() != 2) return false;
  DAGNode* lhs = f->cond->operands[0];
  DAGNode* rhs = f->cond->operands[1];
  if (!(lhs->kind == NodeKind::Variable && lhs->name == d->name)) return false;
  if (rhs->kind != NodeKind::Constant) return false;
  int n = static_cast<int>(rhs->constVal);
  if (begin < 0 || n <= begin || (n - begin) > maxUnroll) return false;

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
  start = begin;
  count = n;
  return true;
}

std::unique_ptr<StmtIR> unrollStmt(IRModule& mod, std::unique_ptr<StmtIR> stmt,
                                   int maxUnroll) {
  if (!stmt) return stmt;

  if (stmt->kind == StmtIRKind::Block) {
    auto* b = static_cast<BlockIR*>(stmt.get());
    std::vector<std::unique_ptr<StmtIR>> flat;
    for (auto& s : b->stmts) {
      auto u = unrollStmt(mod, std::move(s), maxUnroll);
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
    f->init = unrollStmt(mod, std::move(f->init), maxUnroll);
    f->body = unrollStmt(mod, std::move(f->body), maxUnroll);

    std::string var;
    int start = 0, count = 0;
    if (!matchCountedLoop(f, maxUnroll, var, start, count)) return stmt;

    // Count uses on the (already inner-unrolled) loop so the map is current.
    auto globalUses = countUses(stmt.get());

    // Every body-local must be confined to the body. Those that can be inlined
    // are removed; the rest are renamed per iteration so clones never share a
    // mutable variable (which later passes would treat as loop-invariant).
    std::set<std::string> decls;
    collectDeclNames(f->body.get(), decls);
    std::unordered_map<std::string, DAGNode*> declInits;
    collectDeclInits(f->body.get(), declInits);
    auto bodyUses = countUses(f->body.get());
    std::set<std::string> inlinable;
    std::set<std::string> renames;
    for (auto& name : decls) {
      auto g = globalUses.find(name);
      auto b = bodyUses.find(name);
      int gv = (g == globalUses.end()) ? 0 : g->second;
      int bv = (b == bodyUses.end()) ? 0 : b->second;
      if (gv != bv) return stmt;  // used outside the body: cannot unroll
      auto di = declInits.find(name);
      DAGNode* init = (di == declInits.end()) ? nullptr : di->second;
      if (declInlinable(init) && !reassignedIn(f->body.get(), name)) {
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
      // Rename mutable/impure body locals so iterations do not alias.
      for (auto& name : renames) {
        std::string to = name + "__u" + std::to_string(i);
        renameLocal(clone.get(), name, to);
        substituteStmt(mod, clone.get(), name, mod.getVar(to));
      }
      inlineLocalDecls(mod, clone.get(), inlinable);
      block->stmts.push_back(std::move(clone));
    }
    // Unroll loops newly exposed by substituting this loop's variable
    // (e.g. a triangular inner loop whose start depended on the outer var).
    // Recompute use counts: locals were renamed above, so the outer map is stale.
    return unrollStmt(mod, std::move(block), maxUnroll);
  }

  return stmt;
}

}  // namespace

void LoopUnrollPass::run(IRModule& module) {
  if (!module.body) return;
  module.body = unrollStmt(module, std::move(module.body), _maxUnroll);
}

}  // namespace cse
