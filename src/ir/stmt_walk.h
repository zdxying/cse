#pragma once
#include <functional>
#include <string>

#include "dag_node.h"
#include "statement.h"

namespace cse {

// ===== Loop control (`break;` / `continue;`) =====
//
// The frontend has no keyword for these: `break;` arrives as an `ExprStmt`
// whose expression is a bare `Variable` named "break" (likewise "continue").
// That encoding is a property of this IR, and every pass that reasons about
// loop control has to agree on it -- a pass that erases "useless" bare-variable
// statements would silently drop a `break`, and the unroller would splice a
// loop body that still needs it. One place owns the test.
//
// `which`, when given, receives "break" or "continue".
inline bool isLoopControl(const StmtIR* s, std::string* which = nullptr) {
  if (!s || s->kind != StmtIRKind::ExprStmt) return false;
  const DAGNode* e = static_cast<const ExprStmtIR*>(s)->expr;
  if (!e || e->kind != NodeKind::Variable) return false;
  if (e->name == "break" || e->name == "continue") {
    if (which) *which = e->name;
    return true;
  }
  return false;
}

// ===== Expression traversal for passes =====
//
// A pass almost always wants "every expression this statement owns". Writing
// that enumeration by hand at each call site is what made element and member
// stores easy to get wrong: `a[i] = v;` owns *two* expressions -- the lvalue,
// whose index is ordinary arithmetic, and the value being stored -- and a pass
// that lists statement kinds itself tends to remember only the second one. Ten
// copies of that list had drifted apart.
//
// These two functions own the list. `visit` receives a reference to the slot so
// a rewriting pass can assign to it; a pass that only reads can ignore that.
//
// includeLvalue=false is for the one caller that must not treat the lvalue as an
// ordinary expression: CSEPass may not swap it for a variable, because that
// would turn a store into a store to a temporary. Everyone else wants the
// default.
inline void forEachExpr(StmtIR* stmt, const std::function<void(DAGNode*&)>& visit,
                        bool includeLvalue = true) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::ExprStmt:
      visit(static_cast<ExprStmtIR*>(stmt)->expr);
      break;
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (includeLvalue) visit(a->targetExpr);
      visit(a->value);
      break;
    }
    case StmtIRKind::VarDecl:
      visit(static_cast<VarDeclIR*>(stmt)->init);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      visit(f->cond);
      visit(f->update);
      visit(f->updateRhs);
      break;
    }
    case StmtIRKind::IfElse:
      visit(static_cast<IfElseIR*>(stmt)->cond);
      break;
    case StmtIRKind::Return:
      visit(static_cast<ReturnIR*>(stmt)->value);
      break;
    case StmtIRKind::Block:
      break;  // a block holds statements, not expressions
  }
}

// The same, including every nested statement.
inline void forEachExprDeep(StmtIR* stmt,
                            const std::function<void(DAGNode*&)>& visit,
                            bool includeLvalue = true) {
  if (!stmt) return;
  forEachExpr(stmt, visit, includeLvalue);
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) forEachExprDeep(s.get(), visit, includeLvalue);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      forEachExprDeep(f->init.get(), visit, includeLvalue);
      forEachExprDeep(f->body.get(), visit, includeLvalue);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      forEachExprDeep(ie->thenBranch.get(), visit, includeLvalue);
      forEachExprDeep(ie->elseBranch.get(), visit, includeLvalue);
      break;
    }
    // These kinds own no nested statements (their own slots were already
    // visited by forEachExpr above). Listed explicitly so that a new
    // StmtIRKind -- and with it a new place a slot can hide -- is a compile
    // error here rather than a silently missed traversal.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
}

}  // namespace cse
