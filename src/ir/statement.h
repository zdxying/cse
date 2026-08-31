#pragma once
#include "dag_node.h"
#include <memory>
#include <string>
#include <vector>

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

class StmtIR {
public:
    explicit StmtIR(StmtIRKind k) : kind(k) {}
    virtual ~StmtIR() = default;

    StmtIRKind kind;
};

class ExprStmtIR : public StmtIR {
public:
    ExprStmtIR() : StmtIR(StmtIRKind::ExprStmt) {}
    DAGNode* expr = nullptr;
};

class AssignIR : public StmtIR {
public:
    AssignIR() : StmtIR(StmtIRKind::Assign) {}
    std::string target;
    DAGNode* value = nullptr;
};

class VarDeclIR : public StmtIR {
public:
    VarDeclIR() : StmtIR(StmtIRKind::VarDecl) {}
    std::string type;
    std::string name;
    DAGNode* init = nullptr;
};

class ForLoopIR : public StmtIR {
public:
    ForLoopIR() : StmtIR(StmtIRKind::ForLoop) {}
    std::unique_ptr<StmtIR> init;
    DAGNode* cond = nullptr;
    DAGNode* update = nullptr;
    std::unique_ptr<StmtIR> body;
    // Operator for update: '++', '--', '+', '-', '*', '/'
    char updateOp = 0;
    DAGNode* updateRhs = nullptr;
};

class IfElseIR : public StmtIR {
public:
    IfElseIR() : StmtIR(StmtIRKind::IfElse) {}
    DAGNode* cond = nullptr;
    std::unique_ptr<StmtIR> thenBranch;
    std::unique_ptr<StmtIR> elseBranch;
};

class BlockIR : public StmtIR {
public:
    BlockIR() : StmtIR(StmtIRKind::Block) {}
    std::vector<std::unique_ptr<StmtIR>> stmts;
};

class ReturnIR : public StmtIR {
public:
    ReturnIR() : StmtIR(StmtIRKind::Return) {}
    DAGNode* value = nullptr;
};

} // namespace cse
