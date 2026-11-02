// Numerical check that the conservative profile leaves the floating-point
// identities alone.
//
// `x / x`, `0 / x`, `x - x` and `x * 0` are only 1 / 0 / 0 / 0 when the operand
// avoids 0, +-inf and NaN. `x + 0` and `0 - x` are only `x` and `-x` when the
// signed zero is ignored. Under `-s` (which does not set
// allowUnsafeFpIdentities) the generated code must still produce the IEEE-754
// result -- NaN at x = 0 for the division forms, NaN at x = +-inf for the
// subtraction and multiplication forms, and the *correct sign of zero* for the
// additive forms. The well-defined inputs are checked too, so a pass that simply
// stopped simplifying everything would still pass.
#include <cmath>
#include <cstdio>

#include "float_identities.cpp.cse"
#include "harness.h"

static void checkNaN(const char* name, double got) {
  bool ok = std::isnan(got);
  if (!ok) failures++;
  std::printf("%-18s got=%.6f want=NaN  %s\n", name, got, ok ? "OK" : "FAIL");
}

// Zero compares equal regardless of sign, so the sign has to be inspected
// directly: `x + 0.0` at x = -0.0 is +0.0 and `0.0 - x` at x = +0.0 is +0.0.
// A pass that fired the identity would hand back the input's own zero instead.
static void checkZeroSign(const char* name, double got, bool wantNegative) {
  bool ok = (got == 0.0) && (std::signbit(got) == wantNegative);
  if (!ok) failures++;
  std::printf("%-18s got=%s0.0 want=%s0.0  %s\n", name,
              std::signbit(got) ? "-" : "+", wantNegative ? "-" : "+",
              ok ? "OK" : "FAIL");
}

int main() {
  checkNaN("x/x at 0", self_div(0.0));
  checkNaN("0/x at 0", zero_over(0.0));
  check("x/x at 3", self_div(3.0), 1.0);
  check("0/x at 3", zero_over(3.0), 0.0);

  checkNaN("x-x at inf", self_sub(INFINITY));
  checkNaN("x*0 at inf", times_zero(INFINITY));
  checkNaN("x-x at NaN", self_sub(NAN));
  check("x-x at 3", self_sub(3.0), 0.0);
  check("x*0 at 3", times_zero(3.0), 0.0);

  // `x + 0.0` and `0.0 - x` must keep their IEEE result: the sum of -0.0 and
  // +0.0 is +0.0, and the difference of +0.0 and +0.0 is +0.0. If either
  // identity leaked into `-s` the operand's signed zero would come straight
  // back out (the -0.0 case below would return -0.0 and fail).
  checkZeroSign("x+0 at -0", add_zero(-0.0), false);
  checkZeroSign("0-x at +0", zero_minus(0.0), false);
  check("x+0 at 3", add_zero(3.0), 3.0);
  check("0-x at 3", zero_minus(3.0), -3.0);

  // The chained-additive merge is a signed-zero identity as well: it drops the
  // accumulator's leading zero. `y = 0.0; y = y + a; y = y + a;` at a = -0.0 is
  // (0.0 + -0.0) + -0.0 = +0.0, while the merged `y = a + a` would be -0.0.
  checkZeroSign("chain at -0", cleanup_chain(-0.0), false);
  check("chain at 3", cleanup_chain(3.0), 6.0);

  return finish("FLOAT-IDENTITY");
}
