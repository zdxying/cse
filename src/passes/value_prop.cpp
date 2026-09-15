#include "value_prop.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

namespace {

// Check if an expression is trivial to inline (no computation cost to duplicate).
bool isTrivial(DAGNode* e) {
  if (!e) return false;
  switch (e->kind) {
    case NodeKind::Constant:
    case NodeKind::Variable:
      return true;
    case NodeKind::MemberAccess:
    case NodeKind::ArrowAccess:
      return e->operands.size() == 1 && e->operands[0]->kind == NodeKind::Variable;
    default:
      return false;
  }
}

// Collect all variables that are targets of Assign statements (reassigned).
void findReassigned(StmtIR* stmt, std::unordered_set<std::string>& reassigned) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) findReassigned(s.get(), reassigned);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      findReassigned(f->init.get(), reassigned);
      findReassigned(f->body.get(), reassigned);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      findReassigned(ie->thenBranch.get(), reassigned);
      findReassigned(ie->elseBranch.get(), reassigned);
      break;
    }
    case StmtIRKind::Assign:
      reassigned.insert(static_cast<AssignIR*>(stmt)->target);
      break;
    default:
      break;
  }
}

// Substitute variables in a DAG subtree, rebuilding parent nodes as needed.
DAGNode* propExpr(DAGNode* e, IRModule& mod,
                   const std::unordered_map<std::string, DAGNode*>& defs) {
  if (!e) return nullptr;
  if (e->kind == NodeKind::Variable) {
    auto it = defs.find(e->name);
    if (it != defs.end()) return it->second;
    return e;
  }
  bool changed = false;
  std::vector<DAGNode*> newOps;
  for (auto* op : e->operands) {
    DAGNode* r = propExpr(op, mod, defs);
    newOps.push_back(r);
    if (r != op) changed = true;
  }
  if (!changed) return e;
  // Rebuild node through factory for CSE dedup
  switch (e->kind) {
    case NodeKind::BinaryOp:
      return mod.createBinaryOp(e->op, newOps[0], newOps[1]);
    case NodeKind::UnaryOp:
      return mod.createUnaryOp(e->op, newOps[0]);
    case NodeKind::ArrayAccess:
      return mod.createArrayAccess(newOps[0], newOps[1]);
    case NodeKind::MemberAccess:
      return mod.createMemberAccess(newOps[0], e->name);
    case NodeKind::ArrowAccess:
      return mod.createArrowAccess(newOps[0], e->name);
    case NodeKind::Call: {
      std::vector<DAGNode*> args(newOps.begin() + 1, newOps.end());
      return mod.createCall(newOps[0], args);
    }
    default:
      return e;
  }
}

// Walk statements, collect trivial definitions, and inline them.
// Only inlines variables that are never reassigned.
bool walkStmt(StmtIR* stmt,
              const std::unordered_set<std::string>& reassigned,
              std::unordered_map<std::string, DAGNode*>& defs,
              std::vector<std::string>& toRemove) {
  if (!stmt) return false;
  bool changed = false;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) {
        if (s->kind == StmtIRKind::VarDecl) {
          auto* decl = static_cast<VarDeclIR*>(s.get());
          if (decl->init && isTrivial(decl->init) &&
              reassigned.find(decl->name) == reassigned.end()) {
            defs[decl->name] = decl->init;
            toRemove.push_back(decl->name);
          }
        } else if (s->kind == StmtIRKind::Assign) {
          auto* assign = static_cast<AssignIR*>(s.get());
          if (assign->value && isTrivial(assign->value) &&
              reassigned.find(assign->target) == reassigned.end()) {
            defs[assign->target] = assign->value;
          }
        }
        changed |= walkStmt(s.get(), reassigned, defs, toRemove);
      }
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      walkStmt(f->init.get(), reassigned, defs, toRemove);
      walkStmt(f->body.get(), reassigned, defs, toRemove);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      walkStmt(ie->thenBranch.get(), reassigned, defs, toRemove);
      walkStmt(ie->elseBranch.get(), reassigned, defs, toRemove);
      break;
    }
    default:
      break;
  }
  return changed;
}

// Apply substitutions to all expressions in a statement tree.
void applyProp(StmtIR* stmt, IRModule& mod,
               const std::unordered_map<std::string, DAGNode*>& defs) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) applyProp(s.get(), mod, defs);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      applyProp(f->init.get(), mod, defs);
      if (f->cond) f->cond = propExpr(f->cond, mod, defs);
      if (f->update) f->update = propExpr(f->update, mod, defs);
      if (f->updateRhs) f->updateRhs = propExpr(f->updateRhs, mod, defs);
      applyProp(f->body.get(), mod, defs);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (ie->cond) ie->cond = propExpr(ie->cond, mod, defs);
      applyProp(ie->thenBranch.get(), mod, defs);
      applyProp(ie->elseBranch.get(), mod, defs);
      break;
    }
    case StmtIRKind::ExprStmt:
      if (auto* e = static_cast<ExprStmtIR*>(stmt)->expr)
        static_cast<ExprStmtIR*>(stmt)->expr = propExpr(e, mod, defs);
      break;
    case StmtIRKind::Assign:
      if (auto* v = static_cast<AssignIR*>(stmt)->value)
        static_cast<AssignIR*>(stmt)->value = propExpr(v, mod, defs);
      break;
    case StmtIRKind::VarDecl:
      if (auto* i = static_cast<VarDeclIR*>(stmt)->init)
        static_cast<VarDeclIR*>(stmt)->init = propExpr(i, mod, defs);
      break;
    case StmtIRKind::Return:
      if (auto* v = static_cast<ReturnIR*>(stmt)->value)
        static_cast<ReturnIR*>(stmt)->value = propExpr(v, mod, defs);
      break;
  }
}

// Remove inlined VarDecls from a block.
void removeInlined(StmtIR* stmt, const std::vector<std::string>& toRemove) {
  if (!stmt) return;
  if (stmt->kind != StmtIRKind::Block) return;
  auto* b = static_cast<BlockIR*>(stmt);
  auto it = b->stmts.begin();
  while (it != b->stmts.end()) {
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* decl = static_cast<VarDeclIR*>(it->get());
      bool inlined = false;
      for (auto& name : toRemove) {
        if (decl->name == name) { inlined = true; break; }
      }
      if (inlined) { it = b->stmts.erase(it); continue; }
    }
    ++it;
  }
}

}  // namespace

void ValuePropPass::run(IRModule& module) {
  // Collect function parameter names — never inline these
  std::unordered_set<std::string> params;
  for (auto& p : module.funcSig.params) params.insert(p.name);

  // Pre-compute: all variables that are reassigned anywhere in the function
  std::unordered_set<std::string> reassigned;
  findReassigned(module.body.get(), reassigned);

  for (int iter = 0; iter < 5; ++iter) {
    // Don't inline params or reassigned vars
    std::unordered_set<std::string> skip = reassigned;
    for (auto& p : params) skip.insert(p);

    // Collect trivial definitions
    std::unordered_map<std::string, DAGNode*> defs;
    std::vector<std::string> toRemove;
    walkStmt(module.body.get(), skip, defs, toRemove);
    if (defs.empty()) break;

    applyProp(module.body.get(), module, defs);
    removeInlined(module.body.get(), toRemove);
  }
}

}  // namespace cse
