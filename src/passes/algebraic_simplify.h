#pragma once
#include <memory>

#include "pass.h"

namespace cse {

// Algebraic Simplification Pass
//
// Two tiers of rules. The *exact* tier is always applied: `a * 1 -> a`,
// `a / 1 -> a`, `a - 0 -> a`, `--a -> a`, `a + (-b) -> a - b`, `a - (-b) -> a + b`,
// `a * 2 -> a + a`, `(-a)*(-a) -> a*a` and sign normalization -- each bit-exact
// for IEEE-754 including +-0.0 / +-inf / NaN / subnormals. The *assumption* tier
// is gated: reordering/regrouping by the constructor flags below, and the
// special-value identities (`x/x -> 1`, `0/x -> 0`, `x-x -> 0`, `x*0 -> 0`,
// `x+0 -> x`, `0-x -> -x`) by unsafeFpIdentities.
class AlgebraicSimplifyPass : public Pass {
 public:
  // commutative/associative: whether the numeric reordering rules (`a*b == b*a`,
  // `a+(b+c) == (a+b)+c`) may be applied.
  // unsafeFpIdentities: additionally allow `x/x -> 1`, `0/x -> 0`, `x-x -> 0`,
  // `x*0 -> 0`, `x+0 -> x` and `0-x -> -x`, which change the IEEE-754 result
  // for 0 / +-inf / NaN or flip a signed zero.
  // fpReassoc: additionally allow regrouping a multiplication chain, which
  // changes floating-point rounding.
  // assumeFloatOperands: when the numeric kind of an operand cannot be proven,
  // treat it as floating. Only the zero identities consult it, to decide the
  // kind of the constant they substitute -- without it they are skipped rather
  // than risk turning a double division into an integer one.
  //
  // All five default to false: a pass constructed with no arguments still
  // applies the exact tier but no assumption-tier rule. PassManager::createDefault
  // feeds them from CSEConfig.
  explicit AlgebraicSimplifyPass(bool commutative = false,
                                 bool associative = false,
                                 bool unsafeFpIdentities = false,
                                 bool fpReassoc = false,
                                 bool assumeFloatOperands = false)
      : _commutative(commutative),
        _associative(associative),
        _unsafeFpIdentities(unsafeFpIdentities),
        _fpReassoc(fpReassoc),
        _assumeFloatOperands(assumeFloatOperands) {}

  std::string name() const override { return "AlgebraicSimplify"; }
  void run(IRModule& module) override;

 private:
  bool _commutative;
  bool _associative;
  bool _unsafeFpIdentities;
  bool _fpReassoc;
  bool _assumeFloatOperands;
};

inline std::unique_ptr<Pass> createAlgebraicSimplifyPass(
    bool commutative = false, bool associative = false,
    bool unsafeFpIdentities = false, bool fpReassoc = false,
    bool assumeFloatOperands = false) {
  return std::make_unique<AlgebraicSimplifyPass>(commutative, associative,
                                                 unsafeFpIdentities, fpReassoc,
                                                 assumeFloatOperands);
}

}  // namespace cse
