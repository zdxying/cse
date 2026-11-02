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

  // The token stream is *borrowed*, not copied: it must outlive the Parser.
  // Binding it to a temporary would leave `_tokens` referring to a destroyed
  // vector, so that overload is deleted -- the mistake is a compile error
  // rather than undefined behaviour. Keep the `std::vector<Token>` alive for
  // the parser's lifetime.
  explicit Parser(std::vector<Token>&& tokens, const CSEConfig& config = {}) = delete;

  // Parse a CSE-marked function
  std::unique_ptr<FunctionDef> parseFunction();

  // Full parse result
  struct ParseResult {
    std::vector<std::unique_ptr<FunctionDef>> functions;
    std::vector<std::unique_ptr<StructDef>> structDefs;
    std::vector<std::unique_ptr<UsingDecl>> usingDecls;
    std::vector<std::unique_ptr<NamespaceDef>> namespaces;

    // Top-level tokens the parser did not model and skipped over. A non-zero
    // count means the region holds something outside the supported subset, so
    // the code regenerated from `functions`/`structDefs` would be missing it.
    // The caller must pass the region through unchanged rather than emit a
    // partial result. A stray `;` (empty declaration) is not counted.
    size_t skippedTopLevelTokens = 0;
    size_t firstSkippedLine = 0;
    std::string firstSkippedText;

    // Preprocessing directives seen at top level. Directives are not part of
    // the modelled language, so a region containing one cannot be regenerated
    // from this AST: the guard would disappear and `#if 0` code would come back
    // to life. The driver refuses the region before parsing (CSERegion::
    // hasDirectives); this counter is the contract for anyone else.
    size_t directiveCount = 0;
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
  // Lookahead: does the upcoming token sequence look like `<Type> <name>`?
  // Handles qualified/template types (std::array<T,...>, Vector<T,d>) and
  // pointer/reference qualifiers.
  bool looksLikeVarDecl() const;
  // Parse type: handles "double", "int*", "struct Foo", custom names
  std::string parseType();

  // Expression parsing (precedence climbing, low → high)
  std::unique_ptr<Expr> parseExpr();        // entry: = += -= *= /=
  // Depth guard for the recursive paths above. The grammar is a chain of ~14
  // levels per nesting, and every `(`, unary operator and ternary costs another
  // 14 stack frames, so a pathological input (thousands of nested parentheses)
  // overflows the stack and kills the process with no diagnostic. The limit is
  // far above any real kernel and far below the smallest stack we can rely on.
  static constexpr int kMaxExprDepth = 512;
  void enterExpr();
  void leaveExpr() { --_exprDepth; }
  std::unique_ptr<Expr> parseAssignment();  // = += -= *= /=
  std::unique_ptr<Expr> parseTernary();     // a ? b : c
  std::unique_ptr<Expr> parseOr();          // ||
  std::unique_ptr<Expr> parseAnd();         // &&
  std::unique_ptr<Expr> parseBitOr();       // |
  std::unique_ptr<Expr> parseBitXor();      // ^
  std::unique_ptr<Expr> parseBitAnd();      // &
  std::unique_ptr<Expr> parseEquality();    // == !=
  std::unique_ptr<Expr> parseComparison();  // < > <= >=
  std::unique_ptr<Expr> parseShift();       // << >> (two adjacent < / >)
  std::unique_ptr<Expr> parseAddSub();      // + -
  std::unique_ptr<Expr> parseMulDiv();      // * / %
  std::unique_ptr<Expr> parseUnary();       // - + ! ++x --x (type)cast
  std::unique_ptr<Expr> parsePostfix();     // a.b a->b a[i] f(x) x++ x--
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

  // Consume one top-level token the parser does not understand and remember it.
  // A `;` is an empty declaration, not a construct we failed to model, so it is
  // consumed without being recorded.
  void skipUnmodeledTopLevelToken();

  CSEConfig _config;
  const std::vector<Token>& _tokens;
  size_t _pos = 0;

  // Current expression-nesting depth (see kMaxExprDepth).
  int _exprDepth = 0;

  size_t _skippedTopLevel = 0;
  size_t _firstSkippedLine = 0;
  std::string _firstSkippedText;
};

}  // namespace cse
