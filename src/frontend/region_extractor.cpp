#include "region_extractor.h"

#include <sstream>

#include "lexer.h"
#include "token.h"

namespace cse {

namespace {

// Is a line-comment token a `//@cse` marker?
//
// `text` is the raw comment (`//` included). The marker is `//`, optional
// horizontal whitespace, then `@cse`. Only that prefix is structural: trailing
// text is the user's and is preserved verbatim by the driver. Because the token
// stream already separates comments from strings and block comments, a
// `//@cse` inside a string literal or a `/* ... */` can never reach here as a
// line comment.
bool isMarkerComment(const std::string& text) {
  if (text.size() < 2 || text[0] != '/' || text[1] != '/') return false;
  size_t q = 2;
  while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
  return text.compare(q, 4, "@cse") == 0;
}

}  // namespace

// A region is a `//@cse` marker line followed by the code it marks. Boundaries
// are found from the lexer's token stream -- `{`/`}` depth for a `{ ... }`
// body, or a top-level `;` for a declaration -- rather than by counting raw
// characters. This is the same tokenizer the parser runs on, so braces and
// semicolons inside comments, string literals and character literals are
// already gone from the stream and cannot desync the count.
//
// The same stream decides whether the region holds a preprocessing directive
// (`#if`, `#pragma`, ...). That is a token-level question on purpose: a `#` in
// a comment or a string literal is part of that literal's token and must not be
// mistaken for one, while a `#` that follows a comment on the same line *is* a
// directive (C++ replaces a comment with one space before recognising them).
// Scanning raw lines for a leading `#` got both directions wrong.
std::vector<CSERegion> RegionExtractor::extract(const std::string& source) const {
  Lexer lexer(source, CSEConfig{});
  auto tokens = lexer.tokenize(/*keepComments=*/true, /*tolerant=*/true);

  // Split into lines once, so a region's body can be sliced back out of the
  // original text (comments, spacing and all) by line range.
  std::vector<std::string> lines;
  {
    std::istringstream iss(source);
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);
  }

  auto sliceLines = [&lines](size_t firstLine, size_t lastLine) {
    // 1-indexed, inclusive: lines firstLine .. lastLine.
    std::string code;
    for (size_t n = firstLine; n <= lastLine && n >= 1; ++n) {
      if (n - 1 < lines.size()) code += lines[n - 1] + "\n";
    }
    return code;
  };

  std::vector<CSERegion> regions;
  bool inCSE = false;
  size_t cseStart = 0;
  int depth = 0;
  bool sawBrace = false;       // a real opening brace has been seen
  bool topLevelSemi = false;   // a `;` at depth 0, before any `{`
  // Does the region being collected hold a preprocessing directive? A directive
  // is a *token* (TokenType::PreprocessorDirective), so this is decided by the
  // same lexer that decides region boundaries: a `#` inside a string literal or
  // a block comment is part of that token and cannot be mistaken for one.
  bool sawDirective = false;
  size_t prevTokenLine = 0;    // line of the previous token, to detect "leads
                               // the line" (only whitespace before it)

  for (const auto& t : tokens) {
    if (t.type == TokenType::Eof) break;

    if (!inCSE) {
      // A marker must be a line comment that begins its line. Comments inside a
      // block comment are not tokens at all, and a `//@cse` in a string is a
      // String token, so neither can be mistaken for a marker.
      if (t.type == TokenType::LineComment && t.line != prevTokenLine &&
          isMarkerComment(t.text)) {
        inCSE = true;
        cseStart = t.line;
        depth = 0;
        sawBrace = false;
        topLevelSemi = false;
        sawDirective = false;
      }
      prevTokenLine = t.line;
      continue;
    }

    if (t.type == TokenType::PreprocessorDirective) sawDirective = true;

    if (t.type == TokenType::LBrace) {
      ++depth;
      sawBrace = true;
    } else if (t.type == TokenType::RBrace) {
      --depth;
    } else if (t.type == TokenType::Semicolon && !sawBrace) {
      topLevelSemi = true;
    }

    if (depth <= 0 && (sawBrace || topLevelSemi)) {
      regions.push_back(
          {cseStart, t.line, sliceLines(cseStart + 1, t.line), sawDirective});
      inCSE = false;
    }
    prevTokenLine = t.line;
  }

  // A marker whose region never closes (no `}` / `;` before EOF) still yields a
  // region: the parser reports the real error with a position. A marker with an
  // empty body yields nothing, as before.
  if (inCSE) {
    std::string code = sliceLines(cseStart + 1, lines.size());
    if (!code.empty()) {
      regions.push_back({cseStart, lines.size(), code, sawDirective});
    }
  }

  return regions;
}

}  // namespace cse
