// The int/float kind of the constant a zero identity emits.
//
// `(0 * x + 2) / 4` is 0.5 for a double `x` and 0 for an int `x`, so the same
// shape catches a wrong constant kind in either direction: emitting an integer
// zero where the source computed a double turns the division into integer
// division, and emitting `0.0` where the source computed an int turns 0 into
// 0.5.
//
// These are value checks on purpose. FLOP pins cannot see a wrong kind (the fold
// lowers the count identically either way) and no shape grep distinguishes `0`
// from `0.0` where both parse. The complementary half -- proof that the fold
// fired at all, which values cannot see -- is `verify_zero_product_activity.cpp`.
#include "harness.h"

#include "zero_product_kind.cpp.cse"

int main() {
  double one[1] = {1.0};
  double zero[1] = {0.0};

  // A zero product folds, and the fold carries the double kind: 0.5 either way.
  // The value is independent of the load, so the input cannot mask the defect.
  check("float_literal", sf_float_literal(one), 0.5);
  check("float_literal_z", sf_float_literal(zero), 0.5);
  check("integer_literal", sf_integer_literal(one), 0.5);
  check("integer_literal_z", sf_integer_literal(zero), 0.5);

  // A *proven* integer kind must win over the license: 0, not 0.5. If
  // `assumeFloatOperands` ever overrode a known kind this returns 0.5.
  check("int_proven", static_cast<double>(sf_int_proven(3)), 0.0);

  // `x - x` where `x` is a load of unknown kind: folds to a floating zero.
  check("sub_self", sf_sub_self(one), 0.5);

  // Twins agree wherever both compile; the pins above are the stronger check.
  check("twin_float_literal", sf_float_literal(one), ref_float_literal(one));
  check("twin_integer_literal", sf_integer_literal(one),
        ref_integer_literal(one));
  check("twin_int_proven", static_cast<double>(sf_int_proven(3)),
        static_cast<double>(ref_int_proven(3)));
  check("twin_sub_self", sf_sub_self(one), ref_sub_self(one));

  return finish("ZERO-PRODUCT-KIND");
}
