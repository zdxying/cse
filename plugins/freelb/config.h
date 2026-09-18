#pragma once

#include "../../src/frontend/cse_config.h"
#include "cuda_skip.h"

namespace cse {
namespace freelb {

// Create a CSEConfig with FreeLB/CUDA defaults:
// - Skip __xx__ tokens (CUDA annotations like __host__, __device__, __any__)
// - Simplify Type{expr} to just expr (CSE-friendly)
inline CSEConfig createFreeLBConfig() {
  CSEConfig config;
  config.tokenFilter = skipDoubleUnderscoreTokens;
  config.simplifyBraceInit = true;
  return config;
}

}  // namespace freelb
}  // namespace cse
