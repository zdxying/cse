#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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
  config.allowUnsafeFpIdentities = true;

  // FreeLB kernels are never called with two parameters aliasing the same
  // object: the cell, the momenta vectors and the force array are distinct
  // buffers, and a `const Vector&` parameter is read-only for the kernel's
  // lifetime. Asserting it re-enables sharing loads through reference
  // parameters -- without it the aliasing rule makes every reference parameter
  // a non-shareable root, which costs the force kernels ~85% of their CSE
  // (D3Q19: 341 -> 626 flops). Generic C++ must not set this; `-s` keeps it
  // off and stays sound.
  config.noAlias = true;
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

  // ---- Cost model hooks --------------------------------------------------
  const int dim = lat.dim;

  // `Vector<T, N>` -> N (numeric second template argument); `Vector<T, LatSet::d>`
  // or any non-numeric size -> the configured lattice dimension.
  config.vectorLanes = [dim](const std::string& type) -> int {
    if (type.find("Vector") == std::string::npos) return 0;
    auto comma = type.rfind(',');
    if (comma != std::string::npos) {
      size_t i = comma + 1;
      while (i < type.size() && std::isspace((unsigned char)type[i])) ++i;
      size_t j = i;
      while (j < type.size() && std::isdigit((unsigned char)type[j])) ++j;
      if (j > i) return std::stoi(type.substr(i, j - i));
    }
    return dim;
  };

  // Lattice accessors return `Vector<T, d>`; parse `D<n>Q<q>` if present,
  // otherwise fall back to the configured dimension.
  config.callResultLanes = [dim](const std::string& callee) -> int {
    bool lattice = callee.find("latset::") != std::string::npos ||
                   callee.find("lattice::") != std::string::npos;
    // Only direction-vector accessors (`::c`) are vector-valued; weights and
    // other lattice accessors are scalar.
    if (!lattice || callee.find("::c") == std::string::npos) return 0;
    auto pos = callee.find('D');
    if (pos != std::string::npos) {
      size_t i = pos + 1, j = i;
      while (j < callee.size() && std::isdigit((unsigned char)callee[j])) ++j;
      if (j > i) return std::stoi(callee.substr(i, j - i));
    }
    return dim;
  };

  config.binaryOpCost = [](char op, int a, int b, long long& flops) -> int {
    if (op == '*' && a > 0 && b > 0) {  // vector . vector -> scalar dot
      int d = std::max(a, b);
      flops = 2LL * d - 1;
      return 0;
    }
    int L = std::max(a, b);
    if (L > 0) {  // componentwise / scaling -> vector
      flops = L;
      return L;
    }
    flops = 1;  // scalar op
    return 0;
  };

  config.callCost = [dim](const std::string& callee,
                          const std::vector<int>& argLanes) -> long long {
    int d = dim;
    for (int l : argLanes) d = std::max(d, l);
    auto has = [&callee](const char* s) {
      return callee.find(s) != std::string::npos;
    };
    if (has("getnorm2")) return d > 0 ? 2LL * d - 1 : -1;
    if (has("getnorm")) return d > 0 ? 2LL * d : -1;  // norm2 + sqrt
    if (has("getsum")) return d > 0 ? std::max(0LL, (long long)d - 1) : -1;
    if (has("dot")) return d > 0 ? 2LL * d - 1 : -1;
    if (has("latset::") || has("lattice::")) return 0;  // compile-time accessors
    if (has("clear")) return 0;
    if (has("get")) return 0;  // field/component getters
    static const char* kMath[] = {
        "sqrt", "cbrt", "exp",  "log",  "log2", "log10", "sin",  "cos",
        "tan",  "asin", "acos", "atan", "pow",  "fabs",  "abs",  "floor",
        "ceil", "round", "trunc"};
    for (const char* m : kMath)
      if (callee == m) return 1;
    return -1;  // unmodeled
  };

  return config;
}

// Create LatticeConfig from lattice name (e.g., "D3Q19", "D2Q9").
// Returns empty config if name not recognized.
inline LatticeConfig createLatticeConfig(const std::string& name) {
  if (name == "D2Q5") return {"LatSet", "D2Q5", 2, 5, 1.0 / 3.0};
  if (name == "D2Q9") return {"LatSet", "D2Q9", 2, 9, 1.0 / 3.0};
  if (name == "D3Q7") return {"LatSet", "D3Q7", 3, 7, 1.0 / 3.0};
  if (name == "D3Q15") return {"LatSet", "D3Q15", 3, 15, 1.0 / 3.0};
  if (name == "D3Q19") return {"LatSet", "D3Q19", 3, 19, 1.0 / 3.0};
  if (name == "D3Q27") return {"LatSet", "D3Q27", 3, 27, 1.0 / 3.0};
  return {};
}

}  // namespace freelb
}  // namespace cse
