#pragma once
#include <string>

// Token types produced by the Lexer and consumed by the Parser.

namespace cse {

enum class TokenType {
  // Literals
  Number,
  Identifier,
  String,
  Char,  // 'a', '\n', ... (text is the content, delimiters stripped)

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
  Template,
  Using,
  Typename,
  Class,
  Namespace,
  Static,
  Const,
  Inline,
  Unsigned,

  // Operators
  Plus,
  Minus,
  // `++` / `--` are lexed as one token, the way C++ maximal munch does it: with
  // two separate tokens `a++ + b` and `a + +b` are indistinguishable.
  PlusPlus,
  MinusMinus,
  Star,
  Slash,
  Percent,
  Assign,
  PlusAssign,
  MinusAssign,
  StarAssign,
  SlashAssign,
  PercentAssign,
  AmpAssign,
  PipeAssign,
  CaretAssign,
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
  DoubleColon,
  Comma,
  Semicolon,
  Dot,
  Arrow,
  Amp,    // & (reference/address-of, and bitwise AND in an expression)
  Pipe,   // | (bitwise OR; distinct from `Or`, the logical `||`)
  Caret,  // ^ (bitwise XOR)
  Tilde,  // ~ (bitwise NOT)

  // Brackets
  LParen,
  RParen,
  LBrack,
  RBrack,
  LBrace,
  RBrace,

  // Comments. The parser never sees these (tokenize() drops them by default);
  // the region extractor asks for them so it can find `//@cse` markers and
  // skip over comment text without a second, character-level scanner.
  LineComment,   // // ... (text includes the `//`, not the newline)
  BlockComment,  // /* ... */ (text includes the delimiters)

  // A preprocessing directive line: `#` through end of line (text includes the
  // `#`, not the newline). Emitted so a directive can never be *silently*
  // dropped. Directives are outside the language the IR models, and a region
  // regenerated from the AST would delete the guard while resurrecting the code
  // it protects (`#if 0` becoming live). A consumer must therefore treat a
  // region holding one as unmodellable, not ignore it. A `#` inside a comment
  // or string literal never reaches this token -- those are lexed whole first.
  PreprocessorDirective,

  // Special
  Eof,
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
    case TokenType::Char:
      return "Char";
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
    case TokenType::Template:
      return "Template";
    case TokenType::Using:
      return "Using";
    case TokenType::Typename:
      return "Typename";
    case TokenType::Class:
      return "Class";
    case TokenType::Namespace:
      return "Namespace";
    case TokenType::Static:
      return "Static";
    case TokenType::Const:
      return "Const";
    case TokenType::Inline:
      return "Inline";
    case TokenType::Unsigned:
      return "Unsigned";
    case TokenType::Plus:
      return "Plus";
    case TokenType::Minus:
      return "Minus";
    case TokenType::PlusPlus:
      return "PlusPlus";
    case TokenType::MinusMinus:
      return "MinusMinus";
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
    case TokenType::PercentAssign:
      return "PercentAssign";
    case TokenType::AmpAssign:
      return "AmpAssign";
    case TokenType::PipeAssign:
      return "PipeAssign";
    case TokenType::CaretAssign:
      return "CaretAssign";
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
    case TokenType::DoubleColon:
      return "DoubleColon";
    case TokenType::Comma:
      return "Comma";
    case TokenType::Semicolon:
      return "Semicolon";
    case TokenType::Dot:
      return "Dot";
    case TokenType::Arrow:
      return "Arrow";
    case TokenType::Amp:
      return "Amp";
    case TokenType::Pipe:
      return "Pipe";
    case TokenType::Caret:
      return "Caret";
    case TokenType::Tilde:
      return "Tilde";
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
    case TokenType::LineComment:
      return "LineComment";
    case TokenType::BlockComment:
      return "BlockComment";
    case TokenType::PreprocessorDirective:
      return "PreprocessorDirective";
    case TokenType::Eof:
      return "Eof";
  }
  return "Unknown";
}

}  // namespace cse
