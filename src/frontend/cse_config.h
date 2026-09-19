#pragma once
#include <functional>
#include <string>
#include <unordered_map>

#include "token.h"

namespace cse {

class IRModule;
struct DAGNode;

// Configuration for the CSE frontend (lexer + parser) and optimization passes.
// Controls generic C++ behavior, the level of algebraic assumptions the
// optimizer is allowed to make, and optional project-specific hooks.
//
// Defaults are *conservative* (safe for generic C++): the optimizer will not
// assume numeric commutativity/associativity unless the caller opts in.
struct CSEConfig {
  // Token filter: return true to skip a token during lexing.
  // nullptr = no filtering (pass all tokens through).
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

  // ---- Project-specific hooks --------------------------------------------

  // Resolve a source-level name to a constant DAG node (e.g. compile-time
  // numeric members of a project type). Return nullptr to leave the name as a
  // variable. Optional.
  std::function<DAGNode*(IRModule&, const std::string&)> resolveName = nullptr;

  // Lower fixed-size value types (e.g. `Vector<T,N>`) to per-component scalars
  // so componentwise arithmetic (+, -, *, /) and dot products can be CSE'd.
  //   - `vectorDim`: number of components.
  //   - `isVectorType`: classifies a declared type as vector-valued.
  //   - `isVectorProducingCall`: marks a call whose result is a vector (e.g. a
  //     project vector accessor such as `ns::c<T>(k)`).
  bool lowerVectors = false;
  int vectorDim = 0;
  std::function<bool(const std::string&)> isVectorType = nullptr;
  std::function<bool(const std::string&)> isVectorProducingCall = nullptr;

  // Name of the scalar variable holding component `idx` of a lowered vector
  // local, so later passes can fold `v[idx]` to that variable (e.g.
  // `("unew", 1) -> "unew_1"`). Optional.
  std::function<std::string(const std::string&, long long)> vectorLocalName =
      nullptr;

  // Values for non-type template parameters or other compile-time names, e.g.
  // the `unsigned int d` of ScalarForcePopImpl. Folded to constants.
  std::unordered_map<std::string, double> constBindings;
};

}  // namespace cse
