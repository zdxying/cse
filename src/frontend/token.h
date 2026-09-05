#pragma once
#include <cstdint>
#include <string>

// Token types produced by the Lexer and consumed by the Parser.

namespace cse {

enum class TokenType {
  // Literals
  Number,
  Identifier,
  String,

  // Keywords
  For,
  If,
  Else,
  Return,
  Int,
  Double,
  Float,
  Void,
  Struct,

  // Operators
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Assign,
  PlusAssign,
  MinusAssign,
  StarAssign,
  SlashAssign,
  Equal,
  NotEqual,
  Less,
  Greater,
  LessEqual,
  GreaterEqual,
  And,
  Or,
  Not,
  Question,
  Colon,
  Comma,
  Semicolon,
  Dot,
  Arrow,

  // Brackets
  LParen,
  RParen,
  LBrack,
  RBrack,
  LBrace,
  RBrace,

  // Special
  CSEMarker,  // //@cse
  Eof,
  Newline,
};

struct Token {
  TokenType type;
  std::string text;
  size_t line = 0;
  size_t col = 0;
  double numVal = 0;
};

inline const char* tokenTypeName(TokenType t) {
  switch (t) {
    case TokenType::Number:
      return "Number";
    case TokenType::Identifier:
      return "Identifier";
    case TokenType::String:
      return "String";
    case TokenType::For:
      return "For";
    case TokenType::If:
      return "If";
    case TokenType::Else:
      return "Else";
    case TokenType::Return:
      return "Return";
    case TokenType::Int:
      return "Int";
    case TokenType::Double:
      return "Double";
    case TokenType::Float:
      return "Float";
    case TokenType::Void:
      return "Void";
    case TokenType::Struct:
      return "Struct";
    case TokenType::Plus:
      return "Plus";
    case TokenType::Minus:
      return "Minus";
    case TokenType::Star:
      return "Star";
    case TokenType::Slash:
      return "Slash";
    case TokenType::Percent:
      return "Percent";
    case TokenType::Assign:
      return "Assign";
    case TokenType::PlusAssign:
      return "PlusAssign";
    case TokenType::MinusAssign:
      return "MinusAssign";
    case TokenType::StarAssign:
      return "StarAssign";
    case TokenType::SlashAssign:
      return "SlashAssign";
    case TokenType::Equal:
      return "Equal";
    case TokenType::NotEqual:
      return "NotEqual";
    case TokenType::Less:
      return "Less";
    case TokenType::Greater:
      return "Greater";
    case TokenType::LessEqual:
      return "LessEqual";
    case TokenType::GreaterEqual:
      return "GreaterEqual";
    case TokenType::And:
      return "And";
    case TokenType::Or:
      return "Or";
    case TokenType::Not:
      return "Not";
    case TokenType::Question:
      return "Question";
    case TokenType::Colon:
      return "Colon";
    case TokenType::Comma:
      return "Comma";
    case TokenType::Semicolon:
      return "Semicolon";
    case TokenType::Dot:
      return "Dot";
    case TokenType::Arrow:
      return "Arrow";
    case TokenType::LParen:
      return "LParen";
    case TokenType::RParen:
      return "RParen";
    case TokenType::LBrack:
      return "LBrack";
    case TokenType::RBrack:
      return "RBrack";
    case TokenType::LBrace:
      return "LBrace";
    case TokenType::RBrace:
      return "RBrace";
    case TokenType::CSEMarker:
      return "CSEMarker";
    case TokenType::Eof:
      return "Eof";
    case TokenType::Newline:
      return "Newline";
  }
  return "Unknown";
}

}  // namespace cse
