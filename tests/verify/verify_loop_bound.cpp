// A loop whose bound is not exactly integral must keep all of its iterations.
// `for (i = 0; i < 4.5; ++i)` runs i = 0..4, summing to 10; truncating the bound
// to `int` (4) unrolled it to four iterations and returned 6.
#include <cmath>
#include <cstdio>

#include "loop_bound.cpp.cse"
#include "harness.h"

int main() {
  const double got = float_bound();
  const double want = 10.0;  // 0 + 1 + 2 + 3 + 4
  const bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("float_bound got=%.6f want=%.6f  %s\n", got, want, ok ? "OK" : "FAIL");

  return finish("LOOP-BOUND");
}
