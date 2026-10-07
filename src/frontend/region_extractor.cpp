#include "region_extractor.h"

#include <sstream>

namespace cse {

namespace {

// Character-level scan state, carried across lines.
//
// A `{` or `}` only counts if it is code. The extractor used to count raw
// characters, so a brace inside a comment or a string literal moved the count:
// `// the closing brace } of the scope above` closed the region one line early,
// the truncated fragment failed to parse, and the whole region was passed
// through untouched -- the optimizer silently did nothing at all.
struct ScanState {
  bool blockComment = false;  // inside a /* ... */ comment
  bool sawBrace = false;      // a real opening brace has been seen
  bool topLevelSemi = false;  // a `;` at brace depth 0, before any `{`
};

// Net `{` minus `}` contributed by one line of code, ignoring quoted text and
// comments. `st` carries the block-comment state into the next line.
//
// This is deliberately not a full lexer: it only has to be right about where a
// brace is *not*. Anything it cannot decide (an unterminated quote, say) ends at
// the end of the line, and the lexer reports the real error with a position.
int braceDelta(const std::string& line, ScanState& st) {
  int delta = 0;
  const size_t n = line.size();
  size_t i = 0;
  while (i < n) {
    const char c = line[i];
    const char next = (i + 1 < n) ? line[i + 1] : '\0';

    if (st.blockComment) {
      if (c == '*' && next == '/') {
        st.blockComment = false;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }

    // Tested before the quote cases: `// don't` must not start a character
    // literal, and `// "` must not start a string.
    if (c == '/' && next == '/') break;  // line comment: nothing after counts
    if (c == '/' && next == '*') {
      st.blockComment = true;
      i += 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      ++i;
      while (i < n) {
        if (line[i] == '\\') {  // escape: the next character is data
          i += 2;
          continue;
        }
        if (line[i] == quote) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }

    if (c == '{') {
      ++delta;
      st.sawBrace = true;
    } else if (c == '}') {
      --delta;
    } else if (c == ';' && !st.sawBrace) {
      // A top-level statement terminator before any brace: the region is a
      // declaration, not a `{ ... }` body. Without this a marker in front of a
      // declaration (`//@cse` + `double g = 5.0;`) kept accumulating until the
      // next function's body, so the declaration was swallowed into that
      // region and then dropped -- the output no longer declared `g`.
      st.topLevelSemi = true;
    }
    ++i;
  }
  return delta;
}

}  // namespace

std::vector<CSERegion> RegionExtractor::extract(const std::string& source) const {
  std::vector<CSERegion> regions;
  std::istringstream iss(source);
  std::string line;
  size_t lineNum = 0;
  bool inCSE = false;
  size_t cseStart = 0;
  int braceCount = 0;
  std::string cseCode;
  ScanState st;         // state inside the current region
  ScanState fileState;  // comment state outside regions, so a `//@cse` that sits
                        // inside a block comment is not taken for a marker

  while (std::getline(iss, line)) {
    lineNum++;
    std::string trimmed = line;
    size_t first = trimmed.find_first_not_of(" \t");
    if (first != std::string::npos) trimmed = trimmed.substr(first);

    if (!inCSE) {
      if (!fileState.blockComment &&
          (trimmed == "//@cse" || trimmed.find("//@cse") == 0)) {
        inCSE = true;
        cseStart = lineNum;
        braceCount = 0;
        cseCode.clear();
        st = ScanState{};  // each region is scanned as its own text
        continue;
      }
      // Advance the outside comment state so a marker line inside `/* ... */`
      // is skipped. The brace delta is irrelevant here.
      braceDelta(line, fileState);
    } else {
      braceCount += braceDelta(line, st);
      cseCode += line + "\n";

      if (braceCount <= 0 && !cseCode.empty()) {
        if (st.sawBrace || st.topLevelSemi) {
          regions.push_back({cseStart, lineNum, cseCode});
          inCSE = false;
          cseCode.clear();
          fileState = st;  // resume outside scanning from the region's end state
        }
      }
    }
  }

  if (inCSE && !cseCode.empty()) {
    regions.push_back({cseStart, lineNum, cseCode});
  }

  return regions;
}

}  // namespace cse
