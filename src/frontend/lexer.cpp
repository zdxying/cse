#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <exception>

#include "diagnostics.h"

namespace cse {

Lexer::Lexer(const std::string& source, const CSEConfig& config)
    : _config(config), _src(source) {}

std::vector<Token> Lexer::tokenize(bool keepComments, bool tolerant) {
  _keepComments = keepComments;
  _tolerant = tolerant;
  std::vector<Token> tokens;
  while (_pos < _src.size()) {
    skipWhitespace();
    if (_pos >= _src.size()) break;

    char c = peek();

    // Line comment. Emitted as a token when comments are kept (the region
    // extractor needs the marker text); otherwise skipped.
    if (c == '/' && peek2() == '/') {
      if (_keepComments) tokens.push_back(readLineComment());
      else skipLineComment();
      continue;
    }

    // Block comment
    if (c == '/' && peek2() == '*') {
      if (_keepComments) tokens.push_back(readBlockComment());
      else skipBlockComment();
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
      auto tok = readIdentifier();
      // Apply configurable token filter (e.g., skip __xx__ CUDA annotations)
      if (_config.tokenFilter && _config.tokenFilter(tok)) {
        continue;
      }
      tokens.push_back(tok);
      continue;
    }

    // String literal
    if (c == '"') {
      tokens.push_back(readString());
      continue;
    }

    // Character literal. The region extractor must not read a brace or
    // semicolon inside `'{'` as structure, so `'...'` is lexed as one token.
    if (c == '\'') {
      tokens.push_back(readChar());
      continue;
    }

    // Operators and punctuation
    _pos++;
    switch (c) {
      case '+':
        // `++` is tested before `+=`: maximal munch, so `a+++b` is `(a++) + b`.
        if (peek() == '+') {
          _pos++;
          tokens.push_back(makeToken(TokenType::PlusPlus, "++"));
        } else if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::PlusAssign, "+="));
        } else
          tokens.push_back(makeToken(TokenType::Plus, "+"));
        break;
      case '-':
        // `--` first, then `->`, then `-=`: longest match wins.
        if (peek() == '-') {
          _pos++;
          tokens.push_back(makeToken(TokenType::MinusMinus, "--"));
        } else if (peek() == '>') {
          _pos++;
          tokens.push_back(makeToken(TokenType::Arrow, "->"));
        } else if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::MinusAssign, "-="));
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
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::PercentAssign, "%="));
        } else
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
        // `&&` first, then `&=`, then the single `&`: longest match wins.
        if (peek() == '&') {
          _pos++;
          tokens.push_back(makeToken(TokenType::And, "&&"));
        } else if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::AmpAssign, "&="));
        } else {
          tokens.push_back(makeToken(TokenType::Amp, "&"));
        }
        break;
      case '|':
        // `||` first, then `|=`, then the single `|`. A lone `|` used to fall
        // through and be silently dropped -- the byte vanished, so `a | b`
        // lexed as `a b` and every region using bitwise OR was mis-handled.
        if (peek() == '|') {
          _pos++;
          tokens.push_back(makeToken(TokenType::Or, "||"));
        } else if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::PipeAssign, "|="));
        } else {
          tokens.push_back(makeToken(TokenType::Pipe, "|"));
        }
        break;
      case '^':
        if (peek() == '=') {
          _pos++;
          tokens.push_back(makeToken(TokenType::CaretAssign, "^="));
        } else {
          tokens.push_back(makeToken(TokenType::Caret, "^"));
        }
        break;
      case '~':
        tokens.push_back(makeToken(TokenType::Tilde, "~"));
        break;
      case '?':
        tokens.push_back(makeToken(TokenType::Question, "?"));
        break;
      case ':':
        if (peek() == ':') {
          _pos++;
          tokens.push_back(makeToken(TokenType::DoubleColon, "::"));
        } else
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
      case '#':
        // A preprocessing directive. Emitted as one token (text = the whole
        // line, `#` through the newline) instead of being skipped: a silently
        // dropped directive is how `#if 0` bodies came back to life. The
        // frontend cannot model directives, so the consumer decides what to do
        // with the knowledge -- and it now has it.
        tokens.push_back(readDirective());
        break;
      default:
        // `c` has already been consumed. Tolerant mode drops the byte and
        // carries on; strict mode reports it. A lone `|` used to fall through
        // this way -- see the `|` case above.
        if (!_tolerant) {
          throw CSEError({"lex", "CSE-LEX-UNEXPECTED-BYTE", _line, _col,
                          std::string("unexpected character '") + c + "'"});
        }
        break;
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
    } else if (!_keepComments && c == '/' && peek2() == '/') {
      skipLineComment();
    } else if (!_keepComments && c == '/' && peek2() == '*') {
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

// Integer / fraction / exponent / suffix, as one literal.
//
// The old version consumed digits and '.' only, so `1e16` lexed as the number 1
// followed by the identifier `e16` -- every scientific-notation constant in a
// real kernel was a syntax error.
Token Lexer::readNumber() {
  size_t start = _pos;
  size_t startCol = _col;

  // Hexadecimal literal (`0x1F`). Consumed as a single token; the value is
  // parsed base 16. Previously the `0` and `x1F` split into two tokens.
  if (peek() == '0' && (peek2() == 'x' || peek2() == 'X')) {
    advance();  // '0'
    advance();  // 'x'
    while (_pos < _src.size() && std::isxdigit(peek())) advance();
    if (peek() == 'u' || peek() == 'U' || peek() == 'l' || peek() == 'L') advance();
    std::string text = _src.substr(start, _pos - start);
    Token tok;
    tok.type = TokenType::Number;
    tok.text = text;
    tok.numVal = static_cast<double>(std::strtoull(text.c_str(), nullptr, 16));
    tok.line = _line;
    tok.col = startCol;
    return tok;
  }

  while (_pos < _src.size() && std::isdigit(peek())) advance();
  if (peek() == '.') {
    advance();
    while (_pos < _src.size() && std::isdigit(peek())) advance();
  }

  // Exponent. It is only consumed when a digit actually follows, so `1e` (or a
  // number butted up against an identifier) still lexes as `1` + identifier and
  // reports a syntax error at the right place.
  if (peek() == 'e' || peek() == 'E') {
    size_t save = _pos;
    size_t saveCol = _col;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (_pos < _src.size() && std::isdigit(peek())) {
      while (_pos < _src.size() && std::isdigit(peek())) advance();
    } else {
      _pos = save;
      _col = saveCol;
    }
  }

  // The numeric core is everything up to here; a trailing float suffix is part
  // of the literal but not of the value.
  size_t coreEnd = _pos;
  if (peek() == 'f' || peek() == 'F' || peek() == 'l' || peek() == 'L') advance();

  std::string text = _src.substr(start, coreEnd - start);
  // An integer literal with a leading zero is octal (`010` is 8, not 10).
  // Decimal float forms (`0.5`, `0e5`) contain '.'/'e' and are excluded.
  double val = 0;
  bool isOctal = text.size() > 1 && text[0] == '0' &&
                 text.find_first_of(".eE") == std::string::npos;
  try {
    if (isOctal) {
      val = static_cast<double>(std::strtoull(text.c_str(), nullptr, 8));
    } else {
      val = std::stod(text);
    }
  } catch (const std::exception&) {
    // A literal too large for a double (`1e999`) throws out_of_range. In
    // tolerant mode (the region extractor lexing the whole file) fall back to
    // 0; strict mode lets it propagate so the region is reported unreadable.
    if (!_tolerant) throw;
  }
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
  else if (text == "template")
    type = TokenType::Template;
  else if (text == "using")
    type = TokenType::Using;
  else if (text == "typename")
    type = TokenType::Typename;
  else if (text == "class")
    type = TokenType::Class;
  else if (text == "namespace")
    type = TokenType::Namespace;
  else if (text == "static")
    type = TokenType::Static;
  else if (text == "const")
    type = TokenType::Const;
  else if (text == "inline")
    type = TokenType::Inline;
  else if (text == "unsigned")
    type = TokenType::Unsigned;

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

// `'...'`. Escapes are consumed so `'\''` closes correctly. An unterminated
// literal (or a stray apostrophe) stops at the newline; the parser reports the
// real error if the region needs it.
Token Lexer::readChar() {
  size_t startLine = _line;
  size_t startCol = _col;
  advance();  // skip opening '
  std::string text;
  while (_pos < _src.size() && peek() != '\'' && peek() != '\n') {
    if (peek() == '\\') {
      advance();
      text += advance();
    } else {
      text += advance();
    }
  }
  if (_pos < _src.size() && peek() == '\'') advance();  // skip closing '
  Token tok;
  tok.type = TokenType::Char;
  tok.text = text;
  tok.line = startLine;
  tok.col = startCol;
  return tok;
}

// `// ...` up to (not including) the newline. The token's text is the raw
// comment, `//` included, so a consumer can tell it is a line comment and read
// the marker spelling off it.
Token Lexer::readLineComment() {
  size_t start = _pos;
  size_t startLine = _line;
  size_t startCol = _col;
  while (_pos < _src.size() && peek() != '\n') advance();
  Token tok;
  tok.type = TokenType::LineComment;
  tok.text = _src.substr(start, _pos - start);
  tok.line = startLine;
  tok.col = startCol;
  return tok;
}

// `/* ... */`, delimiters included. An unterminated block comment runs to end
// of input, matching skipBlockComment().
Token Lexer::readBlockComment() {
  size_t start = _pos;
  size_t startLine = _line;
  size_t startCol = _col;
  advance();
  advance();  // skip /*
  while (_pos < _src.size()) {
    if (peek() == '*' && peek2() == '/') {
      advance();
      advance();
      break;
    }
    advance();
  }
  Token tok;
  tok.type = TokenType::BlockComment;
  tok.text = _src.substr(start, _pos - start);
  tok.line = startLine;
  tok.col = startCol;
  return tok;
}

// `# ...` up to (not including) the newline. The token's text is the raw
// directive, `#` included, so the region extractor can report it and the driver
// can decide the region as a whole is unmodellable. Because the token starts at
// the `#`, `makeToken()`'s `_col - text.size()` names that byte.
//
// A `#` inside a comment or a string literal is part of that literal's token and
// never gets here. `skipWhitespace()` consumes a block comment before the `#`,
// which is right: C++ replaces a comment with one space before it recognises
// directives, so `/* note */ #if 0` IS a directive.
//
// The lexer does not verify that the `#` is the first token on its line -- that
// would need a per-line flag, and `foo # bar` is ill-formed C++ anyway. Treating
// any such `#` as a directive makes the region pass through unchanged, which is
// the conservative answer for source the frontend could not compile correctly.
Token Lexer::readDirective() {
  // The operator switch has already consumed the `#`, so the token starts one
  // byte back. The scan stops at the newline, leaving `_line`/`_col` on it.
  size_t start = _pos - 1;
  while (_pos < _src.size() && peek() != '\n') advance();
  return makeToken(TokenType::PreprocessorDirective,
                   _src.substr(start, _pos - start));
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
