#pragma once
#include "ast.h"
#include "token.h"
#include <vector>
#include <memory>

namespace cse {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    // Parse a CSE-marked function
    std::unique_ptr<FunctionDef> parseFunction();

    // Full parse result
    struct ParseResult {
        std::vector<std::unique_ptr<FunctionDef>> functions;
    };

    ParseResult parseAll();

private:
    // Helpers
    Token peek() const;
    Token advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    Token expect(TokenType type);

    SourceLoc currentLoc() const;
    bool isTypeKeyword() const;
    std::string parseType();

    // Parsing expressions
    std::unique_ptr<Expr> parseExpr();
    std::unique_ptr<Expr> parseAssignment();
    std::unique_ptr<Expr> parseTernary();
    std::unique_ptr<Expr> parseOr();
    std::unique_ptr<Expr> parseAnd();
    std::unique_ptr<Expr> parseEquality();
    std::unique_ptr<Expr> parseComparison();
    std::unique_ptr<Expr> parseAddSub();
    std::unique_ptr<Expr> parseMulDiv();
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePostfix();
    std::unique_ptr<Expr> parsePrimary();

    // Parsing statements
    std::unique_ptr<Stmt> parseStmt();
    std::unique_ptr<Stmt> parseBlock();
    std::unique_ptr<Stmt> parseFor();
    std::unique_ptr<Stmt> parseIf();
    std::unique_ptr<Stmt> parseVarDecl();
    std::unique_ptr<Stmt> parseReturn();
    std::unique_ptr<Stmt> parseExprStmt();

    // Parse function parameters
    std::vector<FunctionDef::Param> parseParamList();

    const std::vector<Token>& tokens_;
    size_t pos_ = 0;
};

} // namespace cse
