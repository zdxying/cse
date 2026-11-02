#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Preprocessor: extracts //@cse-marked regions from raw source text.
// Uses brace counting to determine function boundaries.
// Non-CSE regions are passed through unchanged by the driver.

namespace cse {

struct CSERegion {
  size_t startLine;  // 1-indexed, the //@cse marker line
  size_t endLine;    // 1-indexed, inclusive: the region's last line
  std::string code;  // the code to optimize
  // Does the region body contain a preprocessor directive? A directive is
  // outside the language the frontend models: regenerating the region from the
  // AST would delete the directive and resurrect the code inside an `#if 0`
  // block. The driver passes such a region through unchanged. Decided from the
  // lexer's PreprocessorDirective token, so a `#` inside a comment or string
  // literal does not count.
  bool hasDirectives = false;
};

struct RegionExtractor {
  std::vector<CSERegion> extract(const std::string& source) const;
};

}  // namespace cse
