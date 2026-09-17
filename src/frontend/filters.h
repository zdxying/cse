#pragma once

#include "token.h"

namespace cse {

// Skip tokens matching __xx__ pattern (e.g., __any__, __host__, __device__, __CUDA_ARCH__).
// This is a common pattern in CUDA code for device/host annotations.
bool skipDoubleUnderscoreTokens(const Token& tok);

}  // namespace cse
