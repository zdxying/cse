#pragma once
#include <string>
#include <vector>
#include <cstddef>

namespace cse {

struct CSERegion {
    size_t startLine;  // 1-indexed
    size_t endLine;    // 1-indexed (exclusive)
    std::string code;  // the code to optimize
};

class RegionExtractor {
public:
    std::vector<CSERegion> extract(const std::string& source) const;
};

} // namespace cse
