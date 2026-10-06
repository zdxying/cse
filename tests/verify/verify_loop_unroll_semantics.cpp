// Differential check that the loop-unroller fixes preserve behavior.
//
// tests/fixtures/loop_unroll_semantics.cpp holds, for each case, a `//@cse`-
// marked function and an unmarked `ref_` twin with the same body. This file
// includes the optimized output and compares the two over many inputs -- the
// miscompiles it guards against (a dropped compound operator, a load moved past
// a store) keep the FLOP count identical, so only the values can see them.
#include <cmath>
#include <cstdio>

#include "loop_unroll_semantics.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

// The store itself must match too: a dropped compound operator changes the
// mutated buffer even when the returned element happens to agree.
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
  const double xs[] = {-3.25, -0.0, 0.0, 1.0, 2.5};
  for (double x : xs) {
    // 1. element compound store in an unrolled loop
    double o1[4] = {1.0, 2.0, 3.0, 4.0};
    double o2[4] = {1.0, 2.0, 3.0, 4.0};
    check("cmp_elem_loop ret", sf_cmp_elem_loop(o1, x), ref_cmp_elem_loop(o2, x));
    checkArr("cmp_elem_loop store", o1, o2, 4);

    // 2. member compound store in an unrolled loop
    LUBox b1[4] = {{1.0}, {2.0}, {3.0}, {4.0}};
    LUBox b2[4] = {{1.0}, {2.0}, {3.0}, {4.0}};
    check("cmp_member_loop ret", sf_cmp_member_loop(b1, x),
          ref_cmp_member_loop(b2, x));
    double g1[4] = {b1[0].v, b1[1].v, b1[2].v, b1[3].v};
    double g2[4] = {b2[0].v, b2[1].v, b2[2].v, b2[3].v};
    checkArr("cmp_member_loop store", g1, g2, 4);

    // 3. a loop-body local read before a store to the same location
    double a1[4] = {5.0, 6.0, 7.0, 8.0};
    double a2[4] = {5.0, 6.0, 7.0, 8.0};
    check("load_then_store", sf_load_then_store(a1), ref_load_then_store(a2));
    checkArr("load_then_store store", a1, a2, 4);

    // 4. the same with a binary initializer
    double c1[4] = {5.0, 6.0, 7.0, 8.0};
    double c2[4] = {5.0, 6.0, 7.0, 8.0};
    check("load_then_store_bin",
          sf_load_then_store_bin(c1, x + 1.0),
          ref_load_then_store_bin(c2, x + 1.0));
    checkArr("load_then_store_bin store", c1, c2, 4);
  }

  std::printf(failures == 0 ? "\nALL LOOP-UNROLL CHECKS PASSED\n"
                            : "\n%d LOOP-UNROLL CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
