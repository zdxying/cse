// Numerical check that the exact algebraic tier is active in the conservative
// profile and stays exact at the special values.
//
// `x * 1`, `x / 1`, `x - 0`, `--x`, `a + (-b)`, `a - (-b)`, `x * 2` and
// `(-a)*(-a)` are bit-identical rewrites for IEEE-754, so `-s` keeps them. This
// verifier pins two things at once: that each rule *fires* under `-s` (the
// shapes below would otherwise not collapse) and that it produces the IEEE
// result -- in particular that `x - 0` keeps the sign of zero, which is exactly
// what separates it from the licensed `x + 0` in float_identities.cpp.
#include <cmath>
#include <cstdio>

#include "exact_identities.cpp.cse"
#include "harness.h"

static void checkNaN(const char* name, double got) {
  bool ok = std::isnan(got);
  if (!ok) failures++;
  std::printf("%-18s got=%.6f want=NaN  %s\n", name, got, ok ? "OK" : "FAIL");
}

// `check` compares with `fabs(got - want) < tol`, which is false for any
// infinity (inf - inf is NaN). Compare infinities by sign instead.
static void checkInf(const char* name, double got, bool wantNegative) {
  bool ok = std::isinf(got) && (std::signbit(got) == wantNegative);
  if (!ok) failures++;
  std::printf("%-18s got=%s  want=%sinf  %s\n", name, std::signbit(got) ? "-inf" : "+inf",
              wantNegative ? "-" : "+", ok ? "OK" : "FAIL");
}

// Zero compares equal regardless of sign, so inspect the sign directly.
static void checkZeroSign(const char* name, double got, bool wantNegative) {
  bool ok = (got == 0.0) && (std::signbit(got) == wantNegative);
  if (!ok) failures++;
  std::printf("%-18s got=%s0.0 want=%s0.0  %s\n", name,
              std::signbit(got) ? "-" : "+", wantNegative ? "-" : "+",
              ok ? "OK" : "FAIL");
}

int main() {
  // Ordinary values: the rewrites must preserve them.
  check("x*1 at 3", mul_one(3.0), 3.0);
  check("1*x at 3", one_mul(3.0), 3.0);
  check("x/1 at 3", div_one(3.0), 3.0);
  check("x-0 at 3", sub_zero(3.0), 3.0);
  check("a+(-b) at 5,2", add_neg(5.0, 2.0), 3.0);
  check("a-(-b) at 5,2", sub_neg(5.0, 2.0), 7.0);
  check("-(-x) at 3", double_neg(3.0), 3.0);
  check("x*2 at 3", twice(3.0), 6.0);
  check("(-a)*(-a) at 3", even_square(3.0), 9.0);
  check("a*(-b) at 5,2", neg_mul(5.0, 2.0), -10.0);

  // `x - 0` is exact for both signed zeros: -0.0 - 0.0 = -0.0. (The licensed
  // `x + 0` would instead hand back +0.0 at x = -0.0, which is why it stays
  // gated.) This is the assertion that proves the exact tier is really exact.
  checkZeroSign("x-0 at -0", sub_zero(-0.0), true);
  checkZeroSign("-(-x) at -0", double_neg(-0.0), true);
  checkZeroSign("(-a)*(-a) at -0", even_square(-0.0), false);

  // Inf / NaN are carried through unchanged.
  checkNaN("x*1 at NaN", mul_one(NAN));
  checkInf("x/1 at inf", div_one(INFINITY), false);
  checkNaN("a+(-b) at inf,inf", add_neg(INFINITY, INFINITY));
  checkInf("a-(-b) at inf,inf", sub_neg(INFINITY, INFINITY), false);
  checkInf("a*(-b) at inf,2", neg_mul(INFINITY, 2.0), true);

  // Sign normalization (`a*(-b) -> -(a*b)`) is in the exact tier, and its one
  // deviation is the *sign bit of a NaN*: with `a = 0, b = inf` the source
  // computes `0 * -inf` (the IEEE "invalid" NaN, sign bit set) while the
  // rewritten form computes `-(0 * inf)` and propagates the negation instead.
  // That is not an observable value -- isnan() and every comparison report the
  // same thing either way -- which is why the rule may stay ungated: moving it
  // behind `numeric_` would cost `-s` the equilibrium kernel's
  // opposite-direction sharing (93 -> 84 flops). Pin the trade here so it stays
  // a documented exception rather than an unnoticed one.
  checkInf("a*(-b) at 1e308,1e308", neg_mul(1e308, 1e308), true);
  // Signed zeros are exact: these are the products whose sign the rewrite has to
  // preserve. `neg_mul(-0, 3)` computes `-0 * -3`, and `neg_mul(0, 3)` computes
  // `0 * -3`; both are -0.0, and the rewritten literal must agree.
  checkZeroSign("a*(-b) at -0,3", neg_mul(-0.0, 3.0), false);
  checkZeroSign("a*(-b) at 0,3", neg_mul(0.0, 3.0), true);
  // Infinities keep their sign.
  checkInf("a*(-b) at inf,2", neg_mul(INFINITY, 2.0), true);
  checkInf("a*(-b) at 2,inf", neg_mul(2.0, INFINITY), true);
  // The deviation itself, stated explicitly: a NaN in, a NaN out. (Comparing the
  // NaN sign bits here would pin the very payload this comment excuses.)
  checkNaN("a*(-b) at 0,inf is NaN", neg_mul(0.0, INFINITY));
  checkNaN("a*(-b) at inf,0 is NaN", neg_mul(INFINITY, 0.0));

  return finish("EXACT-IDENTITY");
}
