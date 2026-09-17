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
  std::vector<Token> tokenize();

 private:
  char peek() const;      // current char
  char peek2() const;     // next char (for two-char operators like //, /*)
  char advance();         // consume current char, advance position
  void skipWhitespace();  // spaces, tabs, newlines, comments
  void skipLineComment();
  void skipBlockComment();

  Token readNumber();      // int or float literal
  Token readIdentifier();  // keyword or user identifier
  Token readString();      // "string literal"

  Token makeToken(TokenType type, const std::string& text = "");

  CSEConfig _config;
  const std::string& _src;
  size_t _pos = 0;
  size_t _line = 1;
  size_t _col = 1;
};

}  // namespace cse
