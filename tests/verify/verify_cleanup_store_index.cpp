// Differential check that the CleanupPass store-index fix preserves behavior.
//
// tests/fixtures/cleanup_store_index.cpp holds, for each case, a `//@cse`-
// marked function and an unmarked `ref_` twin with the same body. This file
// includes the optimized output and compares the two -- return value and the
// mutated buffer -- over many inputs.
//
// The defect it guards against is a declaration read only as a store index
// (`a[x] = v;`): Cleanup's private use counter never looked at the lvalue slot,
// so `x` looked unused and its declaration was deleted. In the plain cases the
// emitted code fails to compile; in the shadow case it compiles and writes the
// wrong slot, so only the values can see it. Both are caught below.
#include <cmath>
#include <cstdio>

#include "cleanup_store_index.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

// The store target must match too: a dropped index declaration can redirect the
// store to a different slot even when the returned element agrees.
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
  for (double x : xs) {
    // 1. index only in the store lvalue
    double a1[3] = {x, x + 1.0, x + 2.0};
    double a2[3] = {x, x + 1.0, x + 2.0};
    check("store_index ret", sf_store_index(a1), ref_store_index(a2));
    checkArr("store_index store", a1, a2, 3);

    // 2. index inside an expression in the lvalue
    double b1[3] = {x, x + 1.0, x + 2.0};
    double b2[3] = {x, x + 1.0, x + 2.0};
    check("store_index_expr ret", sf_store_index_expr(b1),
          ref_store_index_expr(b2));
    checkArr("store_index_expr store", b1, b2, 3);

    // 3. member element store p[i].v
    CSIBox p1[3] = {{x}, {x + 1.0}, {x + 2.0}};
    CSIBox p2[3] = {{x}, {x + 1.0}, {x + 2.0}};
    check("store_member_index ret", sf_store_member_index(p1),
          ref_store_member_index(p2));
    double g1[3] = {p1[0].v, p1[1].v, p1[2].v};
    double g2[3] = {p2[0].v, p2[1].v, p2[2].v};
    checkArr("store_member_index store", g1, g2, 3);

    // 4. silent wrong binding via a shadowing global
    double c1[3] = {x, x + 1.0, x + 2.0};
    double c2[3] = {x, x + 1.0, x + 2.0};
    check("store_index_shadow ret", sf_store_index_shadow(c1),
          ref_store_index_shadow(c2));
    checkArr("store_index_shadow store", c1, c2, 3);
  }

  std::printf(failures == 0 ? "\nALL CLEANUP-STORE-INDEX CHECKS PASSED\n"
                            : "\n%d CLEANUP-STORE-INDEX CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
