#pragma once
#include "ir_module.h"
#include "../frontend/ast.h"
#include <memory>

namespace cse {

// AST → IR transformation
class IRBuilder {
public:
    explicit IRBuilder(IRModule* module);

    // Build IR from a function AST
    void buildFunction(const FunctionDef& func);

    // Build a statement
    std::unique_ptr<StmtIR> buildStmt(const Stmt& stmt);

    // Build an expression (returns DAG node)
    DAGNode* buildExpr(const Expr& expr);

    // Build assignment
    std::unique_ptr<StmtIR> buildAssignment(const std::string& target, DAGNode* value);

private:
    // Expression builders
    DAGNode* buildBinaryOp(const Expr& expr);
    DAGNode* buildUnaryOp(const Expr& expr);
    DAGNode* buildArrayAccess(const Expr& expr);
    DAGNode* buildCall(const Expr& expr);

    IRModule* module_;
};

} // namespace cse
