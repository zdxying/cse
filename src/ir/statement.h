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
  std::string target;          // simple variable target (if targetExpr is null)
  DAGNode* targetExpr = nullptr;  // complex lvalue (array/member element)
  DAGNode* value = nullptr;
};

struct VarDeclIR : StmtIR {
  VarDeclIR() : StmtIR(StmtIRKind::VarDecl) {}
  std::string type;
  std::string name;
  DAGNode* init = nullptr;
};

struct ForLoopIR : StmtIR {
  ForLoopIR() : StmtIR(StmtIRKind::ForLoop) {}
  std::unique_ptr<StmtIR> init;
  DAGNode* cond = nullptr;
  DAGNode* update = nullptr;
  std::unique_ptr<StmtIR> body;
  // Operator for update: '++', '--', '+', '-', '*', '/'
  char updateOp = 0;
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
