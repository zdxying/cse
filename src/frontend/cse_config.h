#pragma once
#include <functional>
#include <string>

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
};

}  // namespace cse
