#pragma once
#include "token.h"
#include <string>
#include <vector>
#include <cstddef>

namespace cse {

class Lexer {
public:
    explicit Lexer(const std::string& source);
    std::vector<Token> tokenize();

private:
    char peek() const;
    char peek2() const;
    char advance();
    void skipWhitespace();
    void skipLineComment();
    void skipBlockComment();

    Token readNumber();
    Token readIdentifier();
    Token readString();

    Token makeToken(TokenType type, const std::string& text = "");

    const std::string& src_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t col_ = 1;
};

} // namespace cse
