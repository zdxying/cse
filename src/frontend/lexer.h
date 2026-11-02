#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "cse_config.h"
#include "token.h"

// Lexer — source text → token stream.
// Skips whitespace and comments; recognizes keywords, identifiers,
// numbers, strings, operators, and punctuation.

namespace cse {

class Lexer {
 public:
  explicit Lexer(const std::string& source, const CSEConfig& config = {});

  // The source is *borrowed*, not copied: it must outlive the Lexer. Binding it
  // to a temporary would leave `_src` referring to an object destroyed at the
  // end of the constructing full-expression, so that overload is deleted and
  // the mistake becomes a compile error rather than undefined behaviour. Keep a
  // named `std::string` alive for the lexer's lifetime.
  explicit Lexer(std::string&& source, const CSEConfig& config = {}) = delete;

  // Tokenize the source. When `keepComments` is false (the default, and what
  // the parser wants) comments are skipped. The region extractor passes true so
  // it can see `//@cse` markers and never has to scan comment text itself.
  //
  // `tolerant` makes the lexer never throw: an unrecognized byte is skipped and
  // an out-of-range numeric literal becomes 0. The extractor uses it because it
  // lexes the *whole file*, including regions and passthrough text the parser
  // will never accept; a strict pass would abort on the first stray byte and
  // lose every region. The parser's own lexer stays strict, so a genuinely
  // unreadable region is still reported with a position.
  std::vector<Token> tokenize(bool keepComments = false, bool tolerant = false);

 private:
  char peek() const;      // current char
  char peek2() const;     // next char (for two-char operators like //, /*)
  char advance();         // consume current char, advance position
  void skipWhitespace();  // spaces, tabs, newlines (and comments unless kept)
  void skipLineComment();
  void skipBlockComment();

  Token readNumber();      // int or float literal
  Token readIdentifier();  // keyword or user identifier
  Token readString();      // "string literal"
  Token readChar();        // 'c' character literal
  Token readLineComment();  // // ... (raw, includes `//`)
  Token readBlockComment(); // /* ... */ (raw, includes delimiters)
  Token readDirective();    // # ... (raw, includes `#`, excludes the newline)

  Token makeToken(TokenType type, const std::string& text = "");

  CSEConfig _config;
  const std::string& _src;
  size_t _pos = 0;
  size_t _line = 1;
  size_t _col = 1;
  bool _keepComments = false;
  bool _tolerant = false;
};

}  // namespace cse
