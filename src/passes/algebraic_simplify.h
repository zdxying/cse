#pragma once
#include <memory>

#include "pass.h"

namespace cse {

// Algebraic Simplification Pass
// Applies:
//   - Commutativity reordering: x * a * x → a * x * x
//   - Identity elimination: a * 1 → a, a + 0 → a
//   - Zero folding: a * 0 → 0
class AlgebraicSimplifyPass : public Pass {
 public:
  // commutative/associative: whether numeric reordering rules (`a*b == b*a`,
  // `a+(b+c) == (a+b)+c`, identity elimination) may be applied. Defaults are
  // aggressive to preserve historical behavior; the generic/safe mode passes
  // false.
  explicit AlgebraicSimplifyPass(bool commutative = true,
                                 bool associative = true)
      : _commutative(commutative), _associative(associative) {}

  std::string name() const override { return "AlgebraicSimplify"; }
  void run(IRModule& module) override;

 private:
  bool _commutative;
  bool _associative;
};

inline std::unique_ptr<Pass> createAlgebraicSimplifyPass(bool commutative = true,
                                                         bool associative = true) {
  return std::make_unique<AlgebraicSimplifyPass>(commutative, associative);
}

}  // namespace cse
