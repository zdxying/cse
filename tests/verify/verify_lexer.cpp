// Lexer token census.
//
// Property under test: no punctuation byte is ever *silently dropped*. Lexing a
// byte must either produce a token or raise a located CSEError -- never consume
// it and emit nothing. A silent drop is the worst failure mode for a frontend:
// the region still "parses" (minus the operator), so the optimizer silently
// produces different code and every value-level test still passes.
//
// The census also pins the multi-character operators that must lex as one token.
// Gaps it currently reports (single `|`, `%=`) are Phase B3 work; run_tests.sh
// treats a census failure as XFAIL until then.

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "frontend/cse_config.h"
#include "frontend/diagnostics.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/token.h"

using namespace cse;

// The Lexer and Parser *borrow* their input (a `const std::string&` /
// `const std::vector<Token>&` member), so the input has to outlive them. Both
// classes delete the rvalue-reference constructor, which turns "pass a
// temporary and dangle" into a compile error. These assertions pin both halves:
// an lvalue is still accepted, a temporary is not.
static_assert(std::is_constructible_v<Lexer, const std::string&, const CSEConfig&>,
              "a Lexer must be constructible from a named (lvalue) source");
static_assert(!std::is_constructible_v<Lexer, std::string, const CSEConfig&>,
              "a Lexer must NOT be constructible from a temporary source "
              "(it would store a dangling reference)");
static_assert(
    std::is_constructible_v<Parser, const std::vector<Token>&, const CSEConfig&>,
    "a Parser must be constructible from a named (lvalue) token stream");
static_assert(!std::is_constructible_v<Parser, std::vector<Token>, const CSEConfig&>,
              "a Parser must NOT be constructible from a temporary token stream "
              "(it would store a dangling reference)");

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::printf("FAIL  %s\n", what.c_str());
  ++failures;
}

int realTokenCount(const std::string& src) {
  Lexer lexer(src, CSEConfig{});
  int n = 0;
  for (const auto& t : lexer.tokenize()) {
    if (t.type != TokenType::Eof) ++n;
  }
  return n;
}

// A byte must not vanish: lexing it yields at least one non-Eof token, or raises
// a located CSEError.
void censusByte(char c) {
  try {
    if (realTokenCount(std::string(1, c)) == 0) {
      fail(std::string("byte '") + c +
           "' is silently dropped (no token and no diagnostic)");
    }
  } catch (const CSEError&) {
    // A located diagnostic is an acceptable outcome for an unsupported byte.
  }
}

void expectOneToken(const std::string& src) {
  try {
    int n = realTokenCount(src);
    if (n != 1) {
      fail("'" + src + "' should lex as exactly one token, got " +
           std::to_string(n));
    }
  } catch (const CSEError& e) {
    fail("'" + src + "' threw: " + e.what());
  }
}

}  // namespace

int main() {
  // Every ASCII punctuation byte that carries meaning in the supported subset.
  // `#` is checked separately below: it now produces a directive token (it used
  // to be skipped, which is the silent-drop this census exists to forbid).
  const char* singles = "+-*/%=!<>&|^~?:,;.()[]{}";
  for (const char* p = singles; *p; ++p) censusByte(*p);

  // `#` must be visible to a consumer. It used to consume the rest of the line
  // and emit nothing, so a `#if 0` guard vanished while its body stayed live.
  {
    const std::string src = "#if 0";
    Lexer lexer(src, CSEConfig{});
    auto toks = lexer.tokenize();
    bool saw = false;
    for (const auto& t : toks) {
      if (t.type == TokenType::PreprocessorDirective) saw = true;
    }
    if (!saw) fail("'#' did not produce a PreprocessorDirective token");
  }
  // A '#' inside a comment or a string is ordinary text, not a directive.
  {
    const char* notDirectives[] = {"// #if 0", "/* #if 0 */", "\"#if 0\""};
    for (const char* text : notDirectives) {
      const std::string src = text;
      Lexer lexer(src, CSEConfig{});
      for (const auto& t : lexer.tokenize(/*keepComments=*/true)) {
        if (t.type == TokenType::PreprocessorDirective) {
          fail(std::string("'") + text +
               "' was mis-lexed as a preprocessing directive");
        }
      }
    }
  }

  // Multi-character operators that must lex as a single token.
  for (const char* op : {"||", "&&", "==", "!=", "<=", ">=", "++", "--", "->",
                         "+=", "-=", "*=", "/=", "%="}) {
    expectOneToken(op);
  }

  if (failures == 0) {
    std::printf("ALL LEXER-CENSUS CHECKS PASSED\n");
    return 0;
  }
  std::printf("lexer census: %d failure(s)\n", failures);
  return 1;
}
