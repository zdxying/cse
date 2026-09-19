#pragma once

#include "../../src/frontend/cse_config.h"
#include "cuda_skip.h"

namespace cse {
namespace freelb {

// Predicate marking FreeLB lattice accessors and reduction helpers as pure.
inline bool isFreeLBPureFunction(const std::string& callee) {
  static const char* kPrefixes[] = {"latset::", "lattice::", "getnorm2",
                                    "getsum",   "getnorm",   "dot"};
  for (const char* p : kPrefixes) {
    if (callee.find(p) != std::string::npos) return true;
  }
  return false;
}

// Create a CSEConfig with FreeLB/CUDA defaults:
// - Skip __xx__ tokens (CUDA annotations like __host__, __device__, __any__)
// - Simplify Type{expr} to just expr (CSE-friendly)
// - Allow aggressive numeric algebraic rules (the kernels operate on real
//   floating-point/complex fields) and treat lattice accessors as pure.
inline CSEConfig createFreeLBConfig() {
  CSEConfig config;
  config.tokenFilter = skipDoubleUnderscoreTokens;
  config.simplifyBraceInit = true;
  config.assumeNumericCommutative = true;
  config.assumeNumericAssociative = true;
  config.allowFpReassoc = true;
  config.noAlias = false;
  config.isPureFunction = isFreeLBPureFunction;
  return config;
}

}  // namespace freelb
}  // namespace cse
