// The shape half of the preprocessor-directive contract.
//
// A value check cannot see the difference between "the region was passed
// through" and "the region was optimized correctly": both compute the same
// number. And a passthrough is only correct if it happens for the right
// regions. Both directions were wrong when the decision was made by scanning
// raw lines for a leading '#':
//
//   * a comment before the '#' (`/* note */ #if 0`) -- a comment is replaced by
//     one space before directives are recognized, so this IS a directive, but
//     the line does not *start* with '#'; the region was "optimized" and the
//     guarded body came back to life;
//   * a block comment containing a line that starts with `#if 0` -- no
//     directive exists, but the raw scan saw one and refused to optimize an
//     ordinary region.
//
// The decision is now made by the lexer (TokenType::PreprocessorDirective) and
// recorded on CSERegion::hasDirectives. This drives the extractor directly, so
// it pins the token-level contract and not just the driver's behaviour.
//
// Library-level (links libcse.a), like verify_write_set / verify_purity: the
// authority under test is a library component the CLI cannot isolate.
#include <cstdio>
#include <string>

#include "frontend/region_extractor.h"

namespace {

int failures = 0;

void expectDirective(const char* what, const std::string& src, bool want) {
  auto regions = cse::RegionExtractor().extract(src);
  if (regions.size() != 1) {
    std::printf("FAIL  %s: expected 1 region, got %zu\n", what, regions.size());
    ++failures;
    return;
  }
  const bool got = regions[0].hasDirectives;
  if (got != want) {
    std::printf("FAIL  %s: hasDirectives=%d, want %d\n", what,
                static_cast<int>(got), static_cast<int>(want));
    ++failures;
    return;
  }
  std::printf("OK    %s\n", what);
}

}  // namespace

int main() {
  expectDirective("a leading '#' is a directive",
                  "//@cse\ndouble f(double x) {\n#if 0\nreturn -1.0;\n#endif\n"
                  "return x + x;\n}\n",
                  true);
  expectDirective("'#pragma' is a directive",
                  "//@cse\ndouble f(double x) {\n#pragma unroll\n"
                  "return x + x;\n}\n",
                  true);
  // A comment is replaced by one space before directives are recognized, so a
  // '#' that follows a comment on the same line still starts a directive.
  expectDirective("a comment before the '#' does not hide the directive",
                  "//@cse\ndouble f(double x) {\n/* note */ #if 0\n"
                  "return -1.0;\n/* note */ #endif\nreturn x + x;\n}\n",
                  true);
  // The mirror image: '#' inside a comment is text, not a directive.
  expectDirective("a '#' inside a block comment is not a directive",
                  "//@cse\ndouble f(double x) {\n/*\n#if 0\n*/\n"
                  "return x * x + x * x;\n}\n",
                  false);
  expectDirective("a '#' inside a string literal is not a directive",
                  "//@cse\ndouble f(double x) {\nconst char* s = \"#if 0\";\n"
                  "return x + x + s[0];\n}\n",
                  false);
  expectDirective("a '#' inside a line comment is not a directive",
                  "//@cse\ndouble f(double x) {\n// #if 0\nreturn x + x;\n}\n",
                  false);
  // A directive outside the marked region is not the region's problem: the
  // driver only passes through regions whose own body holds one.
  expectDirective("a directive before the marker does not taint the region",
                  "#include <cmath>\n//@cse\ndouble f(double x) {\n"
                  "double a = x * x;\ndouble b = x * x;\nreturn a + b;\n}\n",
                  false);

  if (failures == 0) {
    std::printf("ALL PP-EXTRACTION CHECKS PASSED\n");
    return 0;
  }
  std::printf("pp-extraction: %d failure(s)\n", failures);
  return 1;
}
