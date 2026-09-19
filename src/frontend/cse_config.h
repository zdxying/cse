#pragma once
#include <functional>
#include <string>
#include <unordered_map>

#include "token.h"

namespace cse {

// Configuration for the CSE frontend (lexer + parser) and optimization passes.
// Controls project-specific behavior vs generic C++ behavior, and the level of
// algebraic assumptions the optimizer is allowed to make.
//
// Defaults are *conservative* (safe for generic C++): the optimizer will not
// assume numeric commutativity/associativity unless the caller opts in. The
// FreeLB configuration enables the aggressive rules.
struct CSEConfig {
  // Token filter: return true to skip a token during lexing.
  // nullptr = no filtering (pass all tokens through).
  // Default FreeLB/CUDA: skip __xx__ pattern tokens (__host__, __device__, etc.)
  std::function<bool(const Token&)> tokenFilter = nullptr;

  // If true, desugar Type{expr} to just expr (CSE-friendly).
  // If false, preserve as a constructor/cast expression.
  bool simplifyBraceInit = true;

  // ---- Semantics / safety ------------------------------------------------

  // Assume `+` and `*` are commutative for the operand types. Required for
  // operand reordering and factor extraction. Off by default (unsafe for
  // user-defined operator overloads).
  bool assumeNumericCommutative = false;

  // Assume `+` and `*` are associative. Required for reassociation and
  // constant-factor merging. Off by default.
  bool assumeNumericAssociative = false;

  // Allow reassociating floating-point additions (violates IEEE-754).
  // Only meaningful when assumeNumericAssociative is true.
  bool allowFpReassoc = false;

  // Assume that distinct pointer/array parameters do not alias. When false,
  // any call that may write memory is treated as writing through every
  // non-const pointer.
  bool noAlias = false;

  // Predicate deciding whether a function call is pure (no side effects, result
  // depends only on arguments). Unknown calls are treated as impure. A built-in
  // set of math functions is always considered pure.
  std::function<bool(const std::string&)> isPureFunction = nullptr;

  // ---- FreeLB per-latset instantiation -----------------------------------
  // When `latsetName` is non-empty, the tool is optimizing a template body for
  // one concrete lattice set:
  //   - `<latsetAlias>::q/d/cs2/InvCs2/InvCs4` fold to numbers;
  //   - `latset::c<latsetAlias>` / `latset::w<latsetAlias>` resolve against
  //     `latsetName`, so a templated `LatSet` alias works like a concrete set.
  std::string latsetAlias;  // template alias in the source, e.g. "LatSet"
  std::string latsetName;   // concrete lattice set, e.g. "D3Q19"
  int latsetDim = 0;        // d
  int latsetQ = 0;          // q
  double latsetCs2 = 1.0 / 3.0;

  // Lower FreeLB `Vector<T, LatSet::d>` values to per-component scalars so
  // vector arithmetic (componentwise +,-,*,/ and dot products) can be CSE'd.
  // Set for the force/moment structs; leave off for the equilibrium path.
  bool lowerVectors = false;

  // Values for non-type template parameters or other compile-time names, e.g.
  // the `unsigned int d` of ScalarForcePopImpl. Folded to constants.
  std::unordered_map<std::string, double> constBindings;
};

}  // namespace cse
