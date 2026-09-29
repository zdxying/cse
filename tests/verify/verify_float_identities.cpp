// Numerical check that the conservative profile leaves the floating-point
// identities alone.
//
// `x / x`, `0 / x`, `x - x` and `x * 0` are only 1 / 0 / 0 / 0 when the operand
// avoids 0, +-inf and NaN. Under `-s` (which does not set
// allowUnsafeFpIdentities) the generated code must still produce the IEEE-754
// result -- NaN at x = 0 for the division forms, NaN at x = +-inf for the
// subtraction and multiplication forms. The well-defined inputs are checked too,
// so a pass that simply stopped simplifying everything would still pass.
#include <cmath>
#include <cstdio>

#include "float_identities.cpp.cse"

static int failures = 0;

static void checkNaN(const char* name, double got) {
  bool ok = std::isnan(got);
  if (!ok) failures++;
  std::printf("%-18s got=%.6f want=NaN  %s\n", name, got, ok ? "OK" : "FAIL");
}

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-18s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
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

  std::printf(failures == 0 ? "\nALL FLOAT-IDENTITY CHECKS PASSED\n"
                            : "\n%d FLOAT-IDENTITY CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
