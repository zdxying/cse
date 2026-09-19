#include "counter_prop.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

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
};

void collectVarNamesExpr(DAGNode* e, std::unordered_set<std::string>& out) {
  if (!e) return;
  if (e->kind == NodeKind::Variable && !e->name.empty()) out.insert(e->name);
  for (auto* op : e->operands) collectVarNamesExpr(op, out);
}

void collectVarNamesStmt(StmtIR* s, std::unordered_set<std::string>& out) {
  if (!s) return;
  switch (s->kind) {
    case StmtIRKind::Block:
      for (auto& x : static_cast<BlockIR*>(s)->stmts)
        collectVarNamesStmt(x.get(), out);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(s);
      collectVarNamesStmt(f->init.get(), out);
      collectVarNamesExpr(f->cond, out);
      collectVarNamesExpr(f->update, out);
      collectVarNamesExpr(f->updateRhs, out);
      collectVarNamesStmt(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(s);
      collectVarNamesExpr(ie->cond, out);
      collectVarNamesStmt(ie->thenBranch.get(), out);
      collectVarNamesStmt(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::ExprStmt:
      collectVarNamesExpr(static_cast<ExprStmtIR*>(s)->expr, out);
      break;
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(s);
      if (!a->target.empty()) out.insert(a->target);
      collectVarNamesExpr(a->targetExpr, out);
      collectVarNamesExpr(a->value, out);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(s);
      if (!d->name.empty()) out.insert(d->name);
      collectVarNamesExpr(d->init, out);
      break;
    }
    case StmtIRKind::Return:
      collectVarNamesExpr(static_cast<ReturnIR*>(s)->value, out);
      break;
  }
}

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
  switch (n->kind) {
    case NodeKind::BinaryOp:
      if (ops.size() == 2) return ctx.mod.createBinaryOp(n->op, ops[0], ops[1]);
      break;
    case NodeKind::UnaryOp:
      if (ops.size() == 1) return ctx.mod.createUnaryOp(n->op, ops[0]);
      break;
    case NodeKind::ArrayAccess:
      if (ops.size() == 2) return ctx.mod.createArrayAccess(ops[0], ops[1], n->pure);
      break;
    case NodeKind::MemberAccess:
      if (ops.size() == 1) return ctx.mod.createMemberAccess(ops[0], n->name, n->pure);
      break;
    case NodeKind::ArrowAccess:
      if (ops.size() == 1) return ctx.mod.createArrowAccess(ops[0], n->name, n->pure);
      break;
    case NodeKind::Call: {
      std::vector<DAGNode*> args(ops.begin() + 1, ops.end());
      return ctx.mod.createCall(ops[0], args, n->pure);
    }
    default:
      return n;
  }
  return n;
}

void rewriteStmt(StmtIR* stmt, const Ctx& ctx) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) rewriteStmt(s.get(), ctx);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      rewriteStmt(f->init.get(), ctx);
      if (f->cond) f->cond = rewriteIndexes(f->cond, ctx);
      if (f->update) f->update = rewriteIndexes(f->update, ctx);
      if (f->updateRhs) f->updateRhs = rewriteIndexes(f->updateRhs, ctx);
      rewriteStmt(f->body.get(), ctx);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (ie->cond) ie->cond = rewriteIndexes(ie->cond, ctx);
      rewriteStmt(ie->thenBranch.get(), ctx);
      rewriteStmt(ie->elseBranch.get(), ctx);
      break;
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

// Process a block in order: rewrite indices, fold constant `if`s, and update
// the map of known constant counters. Splices folded branches into the block.
void processBlock(BlockIR* block, const Ctx& outer) {
  ConstMap known;
  Ctx ctx{outer.mod, known, outer.names, outer.vecLocal};
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
          if (branch->kind == StmtIRKind::Block) {
            auto* bb = static_cast<BlockIR*>(branch.get());
            for (auto& s : bb->stmts) {
              rewriteStmt(s.get(), ctx);
              out.push_back(std::move(s));
            }
          } else {
            rewriteStmt(branch.get(), ctx);
            out.push_back(std::move(branch));
          }
        }
        continue;
      }
    }

    rewriteStmt(stmt, ctx);

    // Update known constants.
    if (stmt->kind == StmtIRKind::VarDecl) {
      auto* d = static_cast<VarDeclIR*>(stmt);
      if (isConst(d->init)) {
        known[d->name] = d->init->constVal;
      } else {
        known.erase(d->name);
      }
    } else if (stmt->kind == StmtIRKind::ExprStmt) {
      auto* e = static_cast<ExprStmtIR*>(stmt)->expr;
      if (e && e->kind == NodeKind::UnaryOp &&
          (e->name == "++" || e->name == "--") && !e->operands.empty() &&
          e->operands[0]->kind == NodeKind::Variable) {
        auto it = known.find(e->operands[0]->name);
        if (it != known.end()) it->second += (e->name == "++") ? 1.0 : -1.0;
      }
    } else if (stmt->kind == StmtIRKind::Assign) {
      auto* a = static_cast<AssignIR*>(stmt);
      if (a->targetExpr) {
        // no counter update for element writes
      } else if (isConst(a->value)) {
        known[a->target] = a->value->constVal;
      } else {
        known.erase(a->target);
      }
    }

    out.push_back(std::move(stmtPtr));
  }

  // Recurse into nested blocks that survived.
  for (auto& s : out) {
    if (s->kind == StmtIRKind::Block) processBlock(static_cast<BlockIR*>(s.get()), ctx);
    else if (s->kind == StmtIRKind::ForLoop) {
      auto* f = static_cast<ForLoopIR*>(s.get());
      if (f->body && f->body->kind == StmtIRKind::Block)
        processBlock(static_cast<BlockIR*>(f->body.get()), ctx);
    }
  }

  block->stmts = std::move(out);
}

}  // namespace

void CounterPropPass::run(IRModule& module) {
  if (!module.body || module.body->kind != StmtIRKind::Block) return;

  // Collect all variable names so lowered vector locals can be resolved.
  std::unordered_set<std::string> names;
  collectVarNamesStmt(module.body.get(), names);
  ConstMap known;
  Ctx ctx{module, known, names, _vectorLocalName};
  processBlock(static_cast<BlockIR*>(module.body.get()), ctx);
}

}  // namespace cse
