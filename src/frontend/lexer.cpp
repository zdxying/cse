#include "lexer.h"
#include <cctype>
#include <stdexcept>

namespace cse {

Lexer::Lexer(const std::string& source) : src_(source) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (pos_ < src_.size()) {
        skipWhitespace();
        if (pos_ >= src_.size()) break;

        char c = peek();

        // Check for //@cse marker
        if (c == '/' && peek2() == '/') {
            skipLineComment();
            continue;
        }

        // Block comment
        if (c == '/' && peek2() == '*') {
            skipBlockComment();
            continue;
        }

        // Number
        if (std::isdigit(c) || (c == '.' && pos_ + 1 < src_.size() && std::isdigit(src_[pos_ + 1]))) {
            tokens.push_back(readNumber());
            continue;
        }

        // Identifier or keyword
        if (std::isalpha(c) || c == '_') {
            tokens.push_back(readIdentifier());
            continue;
        }

        // String literal
        if (c == '"') {
            tokens.push_back(readString());
            continue;
        }

        // Operators and punctuation
        pos_++;
        switch (c) {
            case '+':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::PlusAssign, "+=")); }
                else tokens.push_back(makeToken(TokenType::Plus, "+"));
                break;
            case '-':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::MinusAssign, "-=")); }
                else if (peek() == '>') { pos_++; tokens.push_back(makeToken(TokenType::Arrow, "->")); }
                else tokens.push_back(makeToken(TokenType::Minus, "-"));
                break;
            case '*':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::StarAssign, "*=")); }
                else tokens.push_back(makeToken(TokenType::Star, "*"));
                break;
            case '/':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::SlashAssign, "/=")); }
                else tokens.push_back(makeToken(TokenType::Slash, "/"));
                break;
            case '%':
                tokens.push_back(makeToken(TokenType::Percent, "%"));
                break;
            case '=':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::Equal, "==")); }
                else tokens.push_back(makeToken(TokenType::Assign, "="));
                break;
            case '!':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::NotEqual, "!=")); }
                else tokens.push_back(makeToken(TokenType::Not, "!"));
                break;
            case '<':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::LessEqual, "<=")); }
                else tokens.push_back(makeToken(TokenType::Less, "<"));
                break;
            case '>':
                if (peek() == '=') { pos_++; tokens.push_back(makeToken(TokenType::GreaterEqual, ">=")); }
                else tokens.push_back(makeToken(TokenType::Greater, ">"));
                break;
            case '&':
                if (peek() == '&') { pos_++; tokens.push_back(makeToken(TokenType::And, "&&")); }
                break;
            case '|':
                if (peek() == '|') { pos_++; tokens.push_back(makeToken(TokenType::Or, "||")); }
                break;
            case '?': tokens.push_back(makeToken(TokenType::Question, "?")); break;
            case ':': tokens.push_back(makeToken(TokenType::Colon, ":")); break;
            case ',': tokens.push_back(makeToken(TokenType::Comma, ",")); break;
            case ';': tokens.push_back(makeToken(TokenType::Semicolon, ";")); break;
            case '.': tokens.push_back(makeToken(TokenType::Dot, ".")); break;
            case '(': tokens.push_back(makeToken(TokenType::LParen, "(")); break;
            case ')': tokens.push_back(makeToken(TokenType::RParen, ")")); break;
            case '[': tokens.push_back(makeToken(TokenType::LBrack, "[")); break;
            case ']': tokens.push_back(makeToken(TokenType::RBrack, "]")); break;
            case '{': tokens.push_back(makeToken(TokenType::LBrace, "{")); break;
            case '}': tokens.push_back(makeToken(TokenType::RBrace, "}")); break;
            default:
                // Skip unknown characters
                break;
        }
    }
    tokens.push_back(makeToken(TokenType::Eof, ""));
    return tokens;
}

char Lexer::peek() const {
    return pos_ < src_.size() ? src_[pos_] : '\0';
}

char Lexer::peek2() const {
    return (pos_ + 1) < src_.size() ? src_[pos_ + 1] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { line_++; col_ = 1; }
    else col_++;
    return c;
}

void Lexer::skipWhitespace() {
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek2() == '/') {
            skipLineComment();
        } else if (c == '/' && peek2() == '*') {
            skipBlockComment();
        } else {
            break;
        }
    }
}

void Lexer::skipLineComment() {
    while (pos_ < src_.size() && peek() != '\n') advance();
}

void Lexer::skipBlockComment() {
    advance(); advance(); // skip /*
    while (pos_ < src_.size()) {
        if (peek() == '*' && peek2() == '/') { advance(); advance(); return; }
        advance();
    }
}

Token Lexer::readNumber() {
    size_t start = pos_;
    size_t startCol = col_;
    while (pos_ < src_.size() && (std::isdigit(peek()) || peek() == '.')) advance();
    std::string text = src_.substr(start, pos_ - start);
    double val = std::stod(text);
    Token tok;
    tok.type = TokenType::Number;
    tok.text = text;
    tok.numVal = val;  // We'll store in a separate field
    tok.line = line_;
    tok.col = startCol;
    return tok;
}

Token Lexer::readIdentifier() {
    size_t start = pos_;
    size_t startCol = col_;
    while (pos_ < src_.size() && (std::isalnum(peek()) || peek() == '_')) advance();
    std::string text = src_.substr(start, pos_ - start);

    TokenType type = TokenType::Identifier;
    if (text == "for") type = TokenType::For;
    else if (text == "if") type = TokenType::If;
    else if (text == "else") type = TokenType::Else;
    else if (text == "return") type = TokenType::Return;
    else if (text == "int") type = TokenType::Int;
    else if (text == "double") type = TokenType::Double;
    else if (text == "float") type = TokenType::Float;
    else if (text == "void") type = TokenType::Void;
    else if (text == "struct") type = TokenType::Struct;

    Token tok;
    tok.type = type;
    tok.text = text;
    tok.line = line_;
    tok.col = startCol;
    return tok;
}

Token Lexer::readString() {
    size_t startCol = col_;
    advance(); // skip opening "
    std::string text;
    while (pos_ < src_.size() && peek() != '"') {
        if (peek() == '\\') { advance(); text += advance(); }
        else text += advance();
    }
    if (pos_ < src_.size()) advance(); // skip closing "
    Token tok;
    tok.type = TokenType::String;
    tok.text = text;
    tok.line = line_;
    tok.col = startCol;
    return tok;
}

Token Lexer::makeToken(TokenType type, const std::string& text) {
    Token tok;
    tok.type = type;
    tok.text = text;
    tok.line = line_;
    tok.col = col_ - text.size();
    return tok;
}

} // namespace cse
