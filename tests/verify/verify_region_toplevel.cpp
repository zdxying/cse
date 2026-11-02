// Compile-and-run check that a top-level declaration marked with `//@cse` is not
// dropped from the output.
//
// tests/fixtures/region_toplevel.cpp marks a scalar and an array before the
// functions that use them; this file includes the optimized output and reads
// each declaration. A dropped one is a compile error, which is the regression.
#include <cmath>
#include <cstdio>

#include "region_toplevel.cpp.cse"
#include "harness.h"

int main() {
  // 1. the global in front of a marked function
  checkNear("rt_apply", rt_apply(2.0), 2.0 * rt_scale);

  // 2. the global in front of an unmarked function
  checkNear("rt_shift", rt_shift(2.0), 2.0 + rt_bias);

  // 3. the brace-initialized array and the function that reads it
  checkNear("rt_tab[0]", rt_tab[0], 1.0);
  checkNear("rt_tab_sum", rt_tab_sum(), 6.0);

  return finish("REGION-TOPLEVEL");
}
