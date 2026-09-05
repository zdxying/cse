#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Preprocessor: extracts //@cse-marked regions from raw source text.
// Uses brace counting to determine function boundaries.
// Non-CSE regions are passed through unchanged by main.cpp.

namespace cse {

struct CSERegion {
  size_t startLine;  // 1-indexed
  size_t endLine;    // 1-indexed (exclusive)
  std::string code;  // the code to optimize
};

struct RegionExtractor {
  std::vector<CSERegion> extract(const std::string& source) const;
};

}  // namespace cse
