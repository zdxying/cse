#pragma once
#include "token.h"
#include <string>
#include <vector>
#include <cstddef>

// Lexer — source text → token stream.
// Skips whitespace and comments; recognizes keywords, identifiers,
// numbers, strings, operators, and punctuation.

namespace cse {

class Lexer {
public:
    explicit Lexer(const std::string& source);
    std::vector<Token> tokenize();

private:
    char peek() const;      // current char
    char peek2() const;     // next char (for two-char operators like //, /*)
    char advance();         // consume current char, advance position
    void skipWhitespace();  // spaces, tabs, newlines, comments
    void skipLineComment();
    void skipBlockComment();

    Token readNumber();     // int or float literal
    Token readIdentifier(); // keyword or user identifier
    Token readString();     // "string literal"

    Token makeToken(TokenType type, const std::string& text = "");

    const std::string& src_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t col_ = 1;
};

} // namespace cse
