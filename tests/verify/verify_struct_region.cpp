// Compile-and-run check that a `//@cse` region holding a pure data struct (or a
// namespace whose only member is one) is emitted rather than dropped.
//
// tests/fixtures/struct_region.cpp marks each such struct; this file includes
// the optimized output and uses every type it defines. If a definition is
// dropped the include no longer compiles, which is the regression.
#include <cmath>
#include <cstdio>

#include "struct_region.cpp.cse"
#include "harness.h"

int main() {
  // 1. the top-level data struct survived
  SRPoint p{1.5, 2.5};
  checkNear("sr_sum", sr_sum(p), 4.0);

  // 2. the namespace's data struct survived
  sr_ns::SRPair q{3.0, 4.0};
  checkNear("sr_prod", sr_prod(q), 12.0);

  return finish("STRUCT-REGION");
}
