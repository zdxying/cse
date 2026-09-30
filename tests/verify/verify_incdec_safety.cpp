// Differential check that a decrement/increment survives the algebraic and
// reassociation passes as itself, not as a negation or an identity.
//
// Before the guard, `(--x) * y` was emitted as `-(x*y)`, `a + (--x)` as `a - x`,
// and `a - (--x)` as `a + x` -- the decrement vanished and the sign flipped.
#include <cmath>
#include <cstdio>

#include "incdec_safety.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // `(--x) * y`: x is decremented once, then multiplied -> (x-1)*y.
  check("predec_mul(5,2)", predec_mul(5.0, 2.0), 4.0 * 2.0);
  check("predec_mul(5.5,2)", predec_mul(5.5, 2.0), 4.5 * 2.0);
  check("predec_mul(-3,2)", predec_mul(-3.0, 2.0), -4.0 * 2.0);

  // `a + (--x)`: a + (x-1).
  check("predec_add(10,5)", predec_add(10.0, 5.0), 10.0 + 4.0);
  check("predec_add(0,1)", predec_add(0.0, 1.0), 0.0);

  // `a - (--x)`: a - (x-1).
  check("sub_predec(10,5)", sub_predec(10.0, 5.0), 10.0 - 4.0);
  check("sub_predec(7,3)", sub_predec(7.0, 3.0), 7.0 - 2.0);

  // `(x--) * y`: the old x is read, then x decremented -> x*y.
  check("postdec_mul(5,2)", postdec_mul(5.0, 2.0), 5.0 * 2.0);

  // `(++x) * y`: x incremented -> (x+1)*y. `++` was never conflated with `+`
  // (there is no unary-plus simplification), but pin it anyway.
  check("preinc_mul(5,2)", preinc_mul(5.0, 2.0), 6.0 * 2.0);

  // `(x++) * y`: old x read -> x*y.
  check("postinc_mul(5,2)", postinc_mul(5.0, 2.0), 5.0 * 2.0);

  std::printf(failures == 0 ? "\nALL INCDEC-SAFETY CHECKS PASSED\n"
                            : "\n%d INCDEC-SAFETY CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
