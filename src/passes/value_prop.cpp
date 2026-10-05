#include "value_prop.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

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
      // Only a load marked shareable (a read-only root) may be copied to a use
      // site. A load through a pointer/reference that may alias a later store is
      // not shareable, so inlining it would move the read past that store.
      return e->pure && e->operands.size() == 1 &&
             e->operands[0]->kind == NodeKind::Variable;
    default:
      return false;
  }
}

// Does `init` read only variables that are never reassigned in this function?
//
// The rewrite copies the initializer to the use site, so it stays valid only if
// every value it depends on is the same there as at the definition. Checking
// that the inlined name itself is never reassigned (done by the caller) is not
// enough: `double t = a; a = b; return t;` would otherwise become
// `a = b; return a;`. Requires that no variable read by `init` is written
// anywhere in the function.
bool initDepsStable(DAGNode* init,
                    const std::unordered_set<std::string>& reassigned) {
  if (!init) return true;
  if (init->kind == NodeKind::Variable) {
    // `"*"` is the wildcard an impure call contributes: it may write any global
    // and anything reachable from an argument, so no variable is stable across
    // it.
    if (reassigned.count("*")) return false;
    if (reassigned.find(init->name) != reassigned.end()) return false;
    return true;
  }
  for (auto* op : init->operands) {
    if (!initDepsStable(op, reassigned)) return false;
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
void walkStmt(StmtIR* stmt,
              const std::unordered_set<std::string>& blocked,
              const std::unordered_set<std::string>& written,
              std::unordered_map<std::string, DAGNode*>& defs,
              std::vector<std::string>& toRemove) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) {
        if (s->kind == StmtIRKind::VarDecl) {
          auto* decl = static_cast<VarDeclIR*>(s.get());
          if (decl->init && isTrivial(decl->init) &&
              blocked.find(decl->name) == blocked.end() &&
              initDepsStable(decl->init, written)) {
            defs[decl->name] = decl->init;
            toRemove.push_back(decl->name);
          }
        }
        walkStmt(s.get(), blocked, written, defs, toRemove);
      }
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      walkStmt(f->init.get(), blocked, written, defs, toRemove);
      walkStmt(f->body.get(), blocked, written, defs, toRemove);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      walkStmt(ie->thenBranch.get(), blocked, written, defs, toRemove);
      walkStmt(ie->elseBranch.get(), blocked, written, defs, toRemove);
      break;
    }
    default:
      break;
  }
}

// Apply substitutions to all expressions in a statement tree.
void applyProp(StmtIR* stmt, IRModule& mod,
               const std::unordered_map<std::string, DAGNode*>& defs) {
  forEachExprDeep(stmt, [&](DAGNode*& e) { e = propExpr(e, mod, defs); });
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

  // Pre-compute: every variable written anywhere in the function (see
  // ir_utils.h:collectWrittenNames). Used both to protect a variable from being
  // inlined itself and, via initDepsStable, to reject initializers that read a
  // variable which is written somewhere.
  std::unordered_set<std::string> reassigned;
  collectWrittenNames(module.body.get(), reassigned);

  for (int iter = 0; iter < 5; ++iter) {
    // Don't inline params or reassigned vars
    std::unordered_set<std::string> skip = reassigned;
    for (auto& p : params) skip.insert(p);

    // Collect trivial definitions
    std::unordered_map<std::string, DAGNode*> defs;
    std::vector<std::string> toRemove;
    walkStmt(module.body.get(), skip, reassigned, defs, toRemove);
    if (defs.empty()) break;

    applyProp(module.body.get(), module, defs);
    removeInlined(module.body.get(), toRemove);
  }
}

}  // namespace cse
