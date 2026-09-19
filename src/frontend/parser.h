#pragma once
#include <memory>
#include <vector>

#include "ast.h"
#include "cse_config.h"
#include "token.h"

// Recursive descent parser — tokens → AST.
// Expression parsing follows C precedence (assignment → ternary → or → and →
// equality → comparison → addsub → muldiv → unary → postfix → primary).

namespace cse {

class Parser {
 public:
  explicit Parser(const std::vector<Token>& tokens, const CSEConfig& config = {});

  // Parse a CSE-marked function
  std::unique_ptr<FunctionDef> parseFunction();

  // Full parse result
  struct ParseResult {
    std::vector<std::unique_ptr<FunctionDef>> functions;
    std::vector<std::unique_ptr<StructDef>> structDefs;
    std::vector<std::unique_ptr<UsingDecl>> usingDecls;
    std::vector<std::unique_ptr<NamespaceDef>> namespaces;
  };

  // Entry point: parse all top-level constructs (functions + structs)
  ParseResult parseAll();

 private:
  // Token navigation
  Token peek() const;
  Token advance();
  bool check(TokenType type) const;
  bool match(TokenType type);
  Token expect(TokenType type);

  SourceLoc currentLoc() const;
  bool isTypeKeyword() const;
  // Parse type: handles "double", "int*", "struct Foo", custom names
  std::string parseType();

  // Expression parsing (precedence climbing, low → high)
  std::unique_ptr<Expr> parseExpr();        // entry: = += -= *= /=
  std::unique_ptr<Expr> parseAssignment();  // = += -= *= /=
  std::unique_ptr<Expr> parseTernary();     // a ? b : c
  std::unique_ptr<Expr> parseOr();          // ||
  std::unique_ptr<Expr> parseAnd();         // &&
  std::unique_ptr<Expr> parseEquality();    // == !=
  std::unique_ptr<Expr> parseComparison();  // < > <= >=
  std::unique_ptr<Expr> parseAddSub();      // + -
  std::unique_ptr<Expr> parseMulDiv();      // * / %
  std::unique_ptr<Expr> parseUnary();       // - ! (type)cast
  std::unique_ptr<Expr> parsePostfix();     // a.b a->b a[i] f(x)
  std::unique_ptr<Expr> parsePrimary();     // literals, identifiers, (expr)

  // Statement parsing
  std::unique_ptr<Stmt> parseStmt();      // dispatch by first token
  std::unique_ptr<Stmt> parseBlock();     // { stmts }
  std::unique_ptr<Stmt> parseFor();       // for (init; cond; update) body
  std::unique_ptr<Stmt> parseIf();        // if (cond) then [else]
  std::unique_ptr<Stmt> parseVarDecl();   // type name [= init];
  std::unique_ptr<Stmt> parseReturn();    // return expr;
  std::unique_ptr<Stmt> parseExprStmt();  // expr;

  std::vector<FunctionDef::Param> parseParamList();     // (type name, ...)
  std::vector<TemplateParam> parseTemplateParams();    // <typename T, int N>
  std::unique_ptr<StructDef> parseStructDef();         // struct name { members }
  std::unique_ptr<UsingDecl> parseUsingDecl();         // using T = Type;
  std::unique_ptr<NamespaceDef> parseNamespaceDef();   // namespace X { ... }
  std::string parseFullType();                         // supports typename and :: qualified names

  CSEConfig _config;
  const std::vector<Token>& _tokens;
  size_t _pos = 0;
};

}  // namespace cse
