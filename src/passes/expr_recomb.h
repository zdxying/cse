#pragma once
#include <memory>

#include "pass.h"

namespace cse {

// Expression Recombination Pass
//
// Factors a common multiplicative term out of an additive chain:
//   a*x + a*y  →  a*(x + y)
//   a*x + b*x  →  (a + b)*x
//   a*x + a    →  a*(x + 1)
//   a*x - a    →  a*(x - 1)
//
// Two deliberate restrictions:
//
//  * Division is never factored. The IR carries no type information, so
//    `a / x + b / x` cannot become `(a+b)/x` without silently changing integer
//    results (3/2 + 1/2 == 1, but (3+1)/2 == 2). It is left untouched.
//  * The matched factor must be pure. The rewrite collapses two textual
//    occurrences into one, so a side-effecting call or a load from a writable
//    location must never become a shared factor.
//
// Some of these rewrites have to move an operand across the operator -- the two
// cross combinations of `a*x ± b*y`, and recognizing the `a + a*x` / `-a + a*x`
// shapes that ReassociatePass leaves behind. Those are gated on `commutative`.
class ExprRecombinePass : public Pass {
 public:
  explicit ExprRecombinePass(bool commutative = false)
      : _commutative(commutative) {}

  std::string name() const override { return "ExprRecombine"; }
  void run(IRModule& module) override;

 private:
  bool _commutative;
};

inline std::unique_ptr<Pass> createExprRecombinePass(bool commutative = false) {
  return std::make_unique<ExprRecombinePass>(commutative);
}

}  // namespace cse
