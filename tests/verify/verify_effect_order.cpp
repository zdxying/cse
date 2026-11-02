// Pins that reassociation preserves the evaluation order of effects.
//
// ReassociatePass sorts an additive chain's terms by use frequency, which
// reorders their evaluation. That is safe only for effect-free terms. With an
// impure term in the chain it is a silent miscompile:
//
//   double state = 1.0;
//   double f() { state = 2.0; return 1.0; }
//   return state + f() + state;      // 1 + 1 + 2 = 4
//
// sorted to `state + state + f()` gives 1 + 1 + 1 = 3. The effect-preservation
// oracle cannot see it: the multiset of effects is identical, only its position
// relative to a read changed.
//
// The checks below drive the *sourced* order through the optimized twin and
// compare against the untouched twin, so a reordering shows up as a value
// difference. Each body resets the global first, so the pairs are independent.
#include "harness.h"

#include "effect_order.cpp.cse"

int main() {
  // `bump_probe` sets the global to 2.0, so the value depends on whether the
  // second read happened before or after the call.
  check("order_0", sf_effect_order_0(), ref_effect_order_0());
  check("order_1", sf_effect_order_1(), ref_effect_order_1());
  check("order_2", sf_effect_order_2(10.0), ref_effect_order_2(10.0));
  check("order_2b", sf_effect_order_2(-1.5), ref_effect_order_2(-1.5));

  // The pure control: reassociation is allowed here, and the value must be
  // unchanged (addition of the same terms in another order differs only by
  // rounding, so this uses exact binary values).
  check("pure_chain", sf_pure_chain(1.0, 2.0, 4.0), ref_pure_chain(1.0, 2.0, 4.0));
  return finish("EFFECT-ORDER");
}
