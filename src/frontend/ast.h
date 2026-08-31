#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

// Abstract Syntax Tree — frontend output, input to IRBuilder.
// Expressions (Expr) and Statements (Stmt) form a tree representing parsed C++ code.
// Top-level constructs: FunctionDef, StructDef.

namespace cse {

struct SourceLoc {
    size_t line = 0;
    size_t col = 0;
};

// ===== Expressions =====
enum class ExprKind {
    Number,
    Variable,
    ArrayAccess,
    MemberAccess,   // a.b
    ArrowAccess,    // a->b
    Call,
    BinaryOp,
    UnaryOp,
    PostfixOp,      // i++, i--
    Ternary,
    Cast,           // (double)x
};

struct Expr {
    ExprKind kind;
    SourceLoc loc;

    // Number
    double numVal = 0;
    std::string numText;

    // Variable / Identifier
    std::string name;

    // ArrayAccess: base[idx1][idx2]...
    std::unique_ptr<Expr> base;
    std::vector<std::unique_ptr<Expr>> indices;

    // MemberAccess / ArrowAccess
    std::string memberName;

    // Call
    std::vector<std::unique_ptr<Expr>> callArgs;

    // BinaryOp
    char op = 0;
    std::unique_ptr<Expr> lhs, rhs;

    // UnaryOp
    std::unique_ptr<Expr> operand;
    bool prefix = true;  // prefix vs postfix

    // Ternary
    std::unique_ptr<Expr> cond, trueExpr, falseExpr;

    // Cast
    std::string castType;

    Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
};

// ===== Statements =====
enum class StmtKind {
    ExprStmt,
    Assignment,
    VarDecl,
    ForLoop,
    IfElse,
    Block,
    Return,
};

struct Stmt {
    StmtKind kind;
    SourceLoc loc;

    // ExprStmt
    std::unique_ptr<Expr> expr;

    // Assignment / VarDecl
    std::string varName;
    std::unique_ptr<Expr> rhs;
    std::string varType;  // for VarDecl

    // VarDecl initializer
    std::unique_ptr<Expr> init;

    // ForLoop
    std::unique_ptr<Stmt> forInit;
    std::unique_ptr<Expr> forCond;
    std::unique_ptr<Expr> forUpdate;
    std::unique_ptr<Stmt> forBody;

    // IfElse
    std::unique_ptr<Expr> ifCond;
    std::unique_ptr<Stmt> ifThen;
    std::unique_ptr<Stmt> ifElse;

    // Block
    std::vector<std::unique_ptr<Stmt>> stmts;

    // Return
    std::unique_ptr<Expr> retExpr;

    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
};

// ===== Top-level =====
struct FunctionDef {
    std::string returnType;
    std::string name;
    struct Param {
        std::string type;
        std::string name;
    };
    std::vector<Param> params;
    std::unique_ptr<Stmt> body;
    SourceLoc loc;
};

struct StructField {
    std::string type;
    std::string name;
};

struct StructDef {
    std::string name;
    std::vector<StructField> fields;
    std::vector<std::unique_ptr<FunctionDef>> methods;
    SourceLoc loc;
};

} // namespace cse
