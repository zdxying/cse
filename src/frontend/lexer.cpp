#include "lexer.h"

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace cse {

Lexer::Lexer(const std::string& source) : _src(source) {}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (_pos < _src.size()) {
    skipWhitespace();
    if (_pos >= _src.size()) break;

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
    if (std::isdigit(c) ||
        (c == '.' && _pos + 1 < _src.size() && std::isdigit(_src[_pos + 1]))) {
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
    _pos++;
    switch (c) {
      case '+':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::PlusAssign, "+="));
        } else
          tokens.push_back(makeToken(TokenType::Plus, "+"));
        break;
      case '-':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::MinusAssign, "-="));
        } else if (peek() == '>') {
          _pos++;
          tokens.push_back(makeToken(TokenType::Arrow, "->"));
        } else
          tokens.push_back(makeToken(TokenType::Minus, "-"));
        break;
      case '*':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::StarAssign, "*="));
        } else
          tokens.push_back(makeToken(TokenType::Star, "*"));
        break;
      case '/':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::SlashAssign, "/="));
        } else
          tokens.push_back(makeToken(TokenType::Slash, "/"));
        break;
      case '%':
        tokens.push_back(makeToken(TokenType::Percent, "%"));
        break;
      case '=':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::Equal, "=="));
        } else
          tokens.push_back(makeToken(TokenType::Assign, "="));
        break;
      case '!':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::NotEqual, "!="));
        } else
          tokens.push_back(makeToken(TokenType::Not, "!"));
        break;
      case '<':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::LessEqual, "<="));
        } else
          tokens.push_back(makeToken(TokenType::Less, "<"));
        break;
      case '>':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::GreaterEqual, ">="));
        } else
          tokens.push_back(makeToken(TokenType::Greater, ">"));
        break;
      case '&':
        if (peek() == '&') {
          _pos++;
          tokens.push_back(makeToken(TokenType::And, "&&"));
        }
        break;
      case '|':
        if (peek() == '|') {
          _pos++;
          tokens.push_back(makeToken(TokenType::Or, "||"));
        }
        break;
      case '?':
        tokens.push_back(makeToken(TokenType::Question, "?"));
        break;
      case ':':
        tokens.push_back(makeToken(TokenType::Colon, ":"));
        break;
      case ',':
        tokens.push_back(makeToken(TokenType::Comma, ","));
        break;
      case ';':
        tokens.push_back(makeToken(TokenType::Semicolon, ";"));
        break;
      case '.':
        tokens.push_back(makeToken(TokenType::Dot, "."));
        break;
      case '(':
        tokens.push_back(makeToken(TokenType::LParen, "("));
        break;
      case ')':
        tokens.push_back(makeToken(TokenType::RParen, ")"));
        break;
      case '[':
        tokens.push_back(makeToken(TokenType::LBrack, "["));
        break;
      case ']':
        tokens.push_back(makeToken(TokenType::RBrack, "]"));
        break;
      case '{':
        tokens.push_back(makeToken(TokenType::LBrace, "{"));
        break;
      case '}':
        tokens.push_back(makeToken(TokenType::RBrace, "}"));
        break;
      default: {
        std::ostringstream oss;
        oss << "Line " << _line << ":" << _col - 1 << " unexpected character '" << c << "'";
        throw std::runtime_error(oss.str());
      }
    }
  }
  tokens.push_back(makeToken(TokenType::Eof, ""));
  return tokens;
}

char Lexer::peek() const { return _pos < _src.size() ? _src[_pos] : '\0'; }

char Lexer::peek2() const { return (_pos + 1) < _src.size() ? _src[_pos + 1] : '\0'; }

char Lexer::advance() {
  char c = _src[_pos++];
  if (c == '\n') {
    _line++;
    _col = 1;
  } else
    _col++;
  return c;
}

void Lexer::skipWhitespace() {
  while (_pos < _src.size()) {
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
  while (_pos < _src.size() && peek() != '\n') advance();
}

void Lexer::skipBlockComment() {
  advance();
  advance();  // skip /*
  while (_pos < _src.size()) {
    if (peek() == '*' && peek2() == '/') {
      advance();
      advance();
      return;
    }
    advance();
  }
}

Token Lexer::readNumber() {
  size_t start = _pos;
  size_t startCol = _col;
  while (_pos < _src.size() && (std::isdigit(peek()) || peek() == '.')) advance();
  std::string text = _src.substr(start, _pos - start);
  double val = std::stod(text);
  Token tok;
  tok.type = TokenType::Number;
  tok.text = text;
  tok.numVal = val;  // We'll store in a separate field
  tok.line = _line;
  tok.col = startCol;
  return tok;
}

Token Lexer::readIdentifier() {
  size_t start = _pos;
  size_t startCol = _col;
  while (_pos < _src.size() && (std::isalnum(peek()) || peek() == '_')) advance();
  std::string text = _src.substr(start, _pos - start);

  TokenType type = TokenType::Identifier;
  if (text == "for")
    type = TokenType::For;
  else if (text == "if")
    type = TokenType::If;
  else if (text == "else")
    type = TokenType::Else;
  else if (text == "return")
    type = TokenType::Return;
  else if (text == "int")
    type = TokenType::Int;
  else if (text == "double")
    type = TokenType::Double;
  else if (text == "float")
    type = TokenType::Float;
  else if (text == "void")
    type = TokenType::Void;
  else if (text == "struct")
    type = TokenType::Struct;

  Token tok;
  tok.type = type;
  tok.text = text;
  tok.line = _line;
  tok.col = startCol;
  return tok;
}

Token Lexer::readString() {
  size_t startCol = _col;
  advance();  // skip opening "
  std::string text;
  while (_pos < _src.size() && peek() != '"') {
    if (peek() == '\\') {
      advance();
      text += advance();
    } else
      text += advance();
  }
  if (_pos < _src.size()) advance();  // skip closing "
  Token tok;
  tok.type = TokenType::String;
  tok.text = text;
  tok.line = _line;
  tok.col = startCol;
  return tok;
}

Token Lexer::makeToken(TokenType type, const std::string& text) {
  Token tok;
  tok.type = type;
  tok.text = text;
  tok.line = _line;
  tok.col = _col - text.size();
  return tok;
}

}  // namespace cse
