#include "region_extractor.h"

#include <sstream>

namespace cse {

std::vector<CSERegion> RegionExtractor::extract(const std::string& source) const {
  std::vector<CSERegion> regions;
  std::istringstream iss(source);
  std::string line;
  size_t lineNum = 0;
  bool inCSE = false;
  size_t cseStart = 0;
  int braceCount = 0;
  std::string cseCode;

  while (std::getline(iss, line)) {
    lineNum++;
    std::string trimmed = line;
    size_t first = trimmed.find_first_not_of(" \t");
    if (first != std::string::npos) trimmed = trimmed.substr(first);

    if (!inCSE) {
      if (trimmed == "//@cse" || trimmed.find("//@cse") == 0) {
        inCSE = true;
        cseStart = lineNum;
        braceCount = 0;
        cseCode.clear();
        continue;
      }
    } else {
      for (char c : line) {
        if (c == '{') braceCount++;
        if (c == '}') braceCount--;
      }
      cseCode += line + "\n";

      if (braceCount <= 0 && !cseCode.empty()) {
        bool hasBraces = false;
        for (char c : cseCode) {
          if (c == '{') {
            hasBraces = true;
            break;
          }
        }
        if (hasBraces) {
          regions.push_back({cseStart, lineNum, cseCode});
          inCSE = false;
          cseCode.clear();
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
