// Differential check that the CleanupPass nested-block recursion preserves
// behavior.
//
// tests/fixtures/cleanup_nested.cpp holds, for each case, a `//@cse`-marked
// function and an unmarked `ref_` twin with the same body. This file includes
// the optimized output and compares the two -- return value and the mutated
// buffer -- over several inputs.
//
// The removal itself (a dead nested declaration, a nested self-assignment) is
// pinned by the shape checks in run_tests.sh; this verifier's job is the other
// half -- proving the recursion did not delete something live or change a
// value. A dropped live declaration shows up here as a compile error or a
// wrong result.
#include <cmath>
#include <cstdio>

#include "cleanup_nested.cpp.cse"
#include "harness.h"

static void checkArr(const char* name, const double* got, const double* want,
                     int n) {
  for (int i = 0; i < n; ++i) {
    if (std::fabs(got[i] - want[i]) >= 1e-9) {
      failures++;
      std::printf("%-24s [%d] got=%.6f want=%.6f  FAIL\n", name, i, got[i],
                  want[i]);
      return;
    }
  }
  std::printf("%-24s [0..%d] match  OK\n", name, n - 1);
}

int main() {
  const double xs[] = {-3.25, 0.0, 1.0, 2.5};
  const int cs[] = {0, 1};
  for (double x : xs) {
    for (int c : cs) {
      double a1[3] = {x, x + 1.0, x + 2.0};
      double a2[3] = {x, x + 1.0, x + 2.0};
      checkNear("nested_if ret", sf_nested_if(a1, c, x), ref_nested_if(a2, c, x));
      checkArr("nested_if store", a1, a2, 3);

      checkNear("nested_used ret", sf_nested_used(x, c), ref_nested_used(x, c));
    }
  }

  const int ns[] = {0, 1, 4};
  for (int n : ns) {
    double b1[4] = {0.0, 0.0, 0.0, 0.0};
    double b2[4] = {0.0, 0.0, 0.0, 0.0};
    checkNear("nested_for ret", sf_nested_for(b1, n, 2.5),
              ref_nested_for(b2, n, 2.5));
    checkArr("nested_for store", b1, b2, 4);
  }

  return finish("CLEANUP-NESTED");
}
