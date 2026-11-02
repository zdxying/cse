#pragma once
#include <memory>
#include <string>
#include <vector>

#include "dag_node.h"

namespace cse {

// ===== Structured IR Statements =====
// Control flow is represented structurally (nested), not as a CFG.
// Expressions within statements are DAGNode* (shared via CSE).

enum class StmtIRKind {
  ExprStmt,
  Assign,
  VarDecl,
  ForLoop,
  IfElse,
  Block,
  Return,
};

struct StmtIR {
  explicit StmtIR(StmtIRKind k) : kind(k) {}
  virtual ~StmtIR() = default;

  StmtIRKind kind;
};

struct ExprStmtIR : StmtIR {
  ExprStmtIR() : StmtIR(StmtIRKind::ExprStmt) {}
  DAGNode* expr = nullptr;
};

struct AssignIR : StmtIR {
  AssignIR() : StmtIR(StmtIRKind::Assign) {}
  // The store target is represented in exactly one of two ways: a bare variable
  // name (the common case, cheap to compare and rename) or an lvalue expression
  // (`a[i]`, `p->f`, `(c ? s : s).x`). Exactly one is set -- see
  // `hasExactlyOneTarget()`.
  //
  // The pair is redundant by nature, so every consumer has to ask which form it
  // is looking at, and a consumer that checks only `targetExpr` silently ignores
  // every store to a plain variable. The invariant is asserted in
  // IRModule::verify() rather than assumed, so "both set" (which would make a
  // pass rewrite one form and leave the other stale) fails loudly.
  std::string target;             // simple variable target (if targetExpr is null)
  DAGNode* targetExpr = nullptr;  // complex lvalue (array/member element)
  DAGNode* value = nullptr;
  // Non-None for a compound store (`a[i] += x`, `p->f *= y`): `value` is the
  // right-hand side and the operator is `compoundOp` + '='. None means a plain
  // `=`. Storing the operator here (rather than lowering to `target = target op
  // value`) keeps a side-effecting lvalue such as `a[i++] += x` evaluated once.
  OpCode compoundOp = OpCode::None;

  // Is the target represented exactly once? True for a bare name (non-empty
  // `target`, null `targetExpr`) or one expression (empty `target`, non-null
  // `targetExpr`). A store with neither is meaningless; one with both is
  // ambiguous.
  bool hasExactlyOneTarget() const {
    return (!target.empty()) != (targetExpr != nullptr);
  }
};

struct VarDeclIR : StmtIR {
  VarDeclIR() : StmtIR(StmtIRKind::VarDecl) {}
  std::string type;
  std::string name;
  // Array-declarator bounds ("[8]"), or empty for a scalar. The codegen spells
  // `type + " " + name + arrayBounds` (C declarator order); classifyDeclaredType
  // reads the `[` out of `type` to classify the storage as an array root.
  std::string arrayBounds;
  DAGNode* init = nullptr;
};

struct ForLoopIR : StmtIR {
  ForLoopIR() : StmtIR(StmtIRKind::ForLoop) {}
  std::unique_ptr<StmtIR> init;
  DAGNode* cond = nullptr;
  DAGNode* update = nullptr;
  std::unique_ptr<StmtIR> body;
  // Operator for update: Inc/Dec (no `updateRhs`), or Assign/Add/Sub/... with a
  // `updateRhs` (`i += k`). None when the update is not in this normalized form.
  OpCode updateOp = OpCode::None;
  DAGNode* updateRhs = nullptr;
};

struct IfElseIR : StmtIR {
  IfElseIR() : StmtIR(StmtIRKind::IfElse) {}
  DAGNode* cond = nullptr;
  std::unique_ptr<StmtIR> thenBranch;
  std::unique_ptr<StmtIR> elseBranch;
  bool isConstexpr = false;
};

struct BlockIR : StmtIR {
  BlockIR() : StmtIR(StmtIRKind::Block) {}
  std::vector<std::unique_ptr<StmtIR>> stmts;
};

struct ReturnIR : StmtIR {
  ReturnIR() : StmtIR(StmtIRKind::Return) {}
  DAGNode* value = nullptr;
};

}  // namespace cse
