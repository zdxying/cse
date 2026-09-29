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
  // `a+(b+c) == (a+b)+c`, identity elimination) may be applied.
  // unsafeFpIdentities: additionally allow `x/x -> 1`, `0/x -> 0`, `x-x -> 0`
  // and `x*0 -> 0`, which change the IEEE-754 result for 0 / +-inf / NaN.
  // fpReassoc: additionally allow regrouping a multiplication chain, which
  // changes floating-point rounding.
  //
  // All four default to false: a pass constructed with no arguments applies no
  // numeric assumptions at all. PassManager::createDefault feeds them from
  // CSEConfig.
  explicit AlgebraicSimplifyPass(bool commutative = false,
                                 bool associative = false,
                                 bool unsafeFpIdentities = false,
                                 bool fpReassoc = false)
      : _commutative(commutative),
        _associative(associative),
        _unsafeFpIdentities(unsafeFpIdentities),
        _fpReassoc(fpReassoc) {}

  std::string name() const override { return "AlgebraicSimplify"; }
  void run(IRModule& module) override;

 private:
  bool _commutative;
  bool _associative;
  bool _unsafeFpIdentities;
  bool _fpReassoc;
};

inline std::unique_ptr<Pass> createAlgebraicSimplifyPass(
    bool commutative = false, bool associative = false,
    bool unsafeFpIdentities = false, bool fpReassoc = false) {
  return std::make_unique<AlgebraicSimplifyPass>(
      commutative, associative, unsafeFpIdentities, fpReassoc);
}

}  // namespace cse
