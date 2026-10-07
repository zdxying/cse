// Compile-and-run check that a top-level declaration marked with `//@cse` is not
// dropped from the output.
//
// tests/fixtures/region_toplevel.cpp marks a scalar and an array before the
// functions that use them; this file includes the optimized output and reads
// each declaration. A dropped one is a compile error, which is the regression.
#include <cmath>
#include <cstdio>

#include "region_toplevel.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-16s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. the global in front of a marked function
  check("rt_apply", rt_apply(2.0), 2.0 * rt_scale);

  // 2. the global in front of an unmarked function
  check("rt_shift", rt_shift(2.0), 2.0 + rt_bias);

  // 3. the brace-initialized array and the function that reads it
  check("rt_tab[0]", rt_tab[0], 1.0);
  check("rt_tab_sum", rt_tab_sum(), 6.0);

  std::printf(failures == 0 ? "\nALL REGION-TOPLEVEL CHECKS PASSED\n"
                            : "\n%d REGION-TOPLEVEL CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
