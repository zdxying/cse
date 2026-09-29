#pragma once
#include <functional>

#include "dag_node.h"
#include "statement.h"

namespace cse {

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
    default:
      break;
  }
}

}  // namespace cse
