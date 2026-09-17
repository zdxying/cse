#pragma once
#include <functional>

#include "token.h"

namespace cse {

// Configuration for the CSE frontend (lexer + parser).
// Controls project-specific behavior vs generic C++ behavior.
struct CSEConfig {
  // Token filter: return true to skip a token during lexing.
  // nullptr = no filtering (pass all tokens through).
  // Default FreeLB/CUDA: skip __xx__ pattern tokens (__host__, __device__, etc.)
  std::function<bool(const Token&)> tokenFilter = nullptr;

  // If true, desugar Type{expr} to just expr (CSE-friendly).
  // If false, preserve as a constructor/cast expression.
  bool simplifyBraceInit = true;
};

}  // namespace cse
