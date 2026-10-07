// Compile-and-run check that a `//@cse` region holding a pure data struct (or a
// namespace whose only member is one) is emitted rather than dropped.
//
// tests/fixtures/struct_region.cpp marks each such struct; this file includes
// the optimized output and uses every type it defines. If a definition is
// dropped the include no longer compiles, which is the regression.
#include <cmath>
#include <cstdio>

#include "struct_region.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-20s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. the top-level data struct survived
  SRPoint p{1.5, 2.5};
  check("sr_sum", sr_sum(p), 4.0);

  // 2. the namespace's data struct survived
  sr_ns::SRPair q{3.0, 4.0};
  check("sr_prod", sr_prod(q), 12.0);

  std::printf(failures == 0 ? "\nALL STRUCT-REGION CHECKS PASSED\n"
                            : "\n%d STRUCT-REGION CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
