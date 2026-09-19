#pragma once

#include <string>

#include "../../src/frontend/cse_config.h"
#include "../../src/ir/ir_module.h"
#include "cuda_skip.h"

namespace cse {
namespace freelb {

// Per-latset instantiation context for the .ur.h generator. When `name` is
// non-empty, a template body is optimized for one concrete lattice set.
struct LatticeConfig {
  std::string alias;  // template alias in the source, e.g. "LatSet"
  std::string name;   // concrete lattice set, e.g. "D3Q19"
  int dim = 0;        // d
  int q = 0;          // q
  double cs2 = 1.0 / 3.0;
};

// Predicate marking FreeLB lattice accessors and reduction helpers as pure.
inline bool isFreeLBPureFunction(const std::string& callee) {
  static const char* kPrefixes[] = {"latset::", "lattice::", "getnorm2",
                                    "getsum",   "getnorm",   "dot"};
  for (const char* p : kPrefixes) {
    if (callee.find(p) != std::string::npos) return true;
  }
  return false;
}

// Fold `<alias>::q/d/cs2/InvCs2/InvCs4` to a constant. Returns nullptr when no
// per-latset context is configured or the name does not match.
inline DAGNode* resolveLatsetConst(IRModule& mod, const std::string& name,
                                   const LatticeConfig& lat) {
  if (lat.alias.empty() || lat.name.empty()) return nullptr;
  const std::string prefix = lat.alias + "::";
  if (name.compare(0, prefix.size(), prefix) != 0) return nullptr;
  const std::string member = name.substr(prefix.size());

  if (member == "q")
    return mod.createConst(lat.q, std::to_string(lat.q));
  if (member == "d")
    return mod.createConst(lat.dim, std::to_string(lat.dim));
  const double cs2 = lat.cs2;
  if (member == "cs2") return mod.createConst(cs2);
  if (member == "InvCs2") return mod.createConst(1.0 / cs2);
  if (member == "InvCs4") return mod.createConst(1.0 / (cs2 * cs2));
  return nullptr;
}

// Create a CSEConfig with FreeLB/CUDA defaults:
// - Skip __xx__ tokens (CUDA annotations like __host__, __device__, __any__)
// - Simplify Type{expr} to just expr (CSE-friendly)
// - Allow aggressive numeric algebraic rules (the kernels operate on real
//   floating-point/complex fields) and treat lattice accessors as pure
// - Resolve per-latset constants and lower `Vector<T, LatSet::d>` arithmetic
//   when a `LatticeConfig` context is supplied.
inline CSEConfig createFreeLBConfig(const LatticeConfig& lat = {}) {
  CSEConfig config;
  config.tokenFilter = skipDoubleUnderscoreTokens;
  config.simplifyBraceInit = true;
  config.assumeNumericCommutative = true;
  config.assumeNumericAssociative = true;
  config.allowFpReassoc = true;
  config.noAlias = false;
  config.isPureFunction = isFreeLBPureFunction;

  config.resolveName = [lat](IRModule& mod, const std::string& name) {
    return resolveLatsetConst(mod, name, lat);
  };
  config.vectorDim = lat.dim;
  config.isVectorType = [](const std::string& type) {
    return type.find("Vector") != std::string::npos;
  };
  config.isVectorProducingCall = [](const std::string& callee) {
    return callee.find("latset::") != std::string::npos &&
           callee.find("::c") != std::string::npos;
  };
  config.vectorLocalName = [](const std::string& base, long long idx) {
    return base + "_" + std::to_string(idx);
  };
  return config;
}

}  // namespace freelb
}  // namespace cse
