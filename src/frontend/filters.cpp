#include "filters.h"

namespace cse {

bool skipDoubleUnderscoreTokens(const Token& tok) {
  const auto& text = tok.text;
  // Pattern: __xx__ (starts with __, ends with __, at least 4 chars)
  if (text.size() >= 4 && text[0] == '_' && text[1] == '_' &&
      text[text.size() - 1] == '_' && text[text.size() - 2] == '_') {
    return true;
  }
  return false;
}

}  // namespace cse
