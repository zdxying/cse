// Differential check that a store to a reference is no longer deleted.
//
// tests/fixtures/ref_param_store.cpp holds, for each case, a `//@cse`-marked
// function and an unmarked `ref_` twin with the same body. This file includes
// the optimized output and runs both over many inputs, comparing the referent
// *after* the call as well as the return value -- the defect deleted the store,
// so the referent was left untouched and only the side channel catches it.
//
// The pointer case at the end pins the other direction: a pointer parameter's
// own value is a local copy, so that store must still be removable. Its check is
// in run_tests.sh (a shape assertion), because "removed" and "kept" are both
// correct there and no value comparison can tell them apart.
#include <cmath>
#include <cstdio>

#include "ref_param_store.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

static void checkInt(const char* name, int got, int want) {
  bool ok = got == want;
  if (!ok) failures++;
  std::printf("%-24s got=%d want=%d  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  const double xs[] = {-3.5, -1.0, 0.0, 1.0, 2.25, 7.0};
  for (double x : xs) {
    // 1. plain store to a reference parameter, never read back
    double v1 = 100.0, v2 = 100.0;
    check("ref_param ret", sf_ref_param(v1, x), ref_ref_param(v2, x));
    check("ref_param referent", v1, v2);

    // 2. store that is read back
    double r1 = 100.0, r2 = 100.0;
    check("ref_param_read ret", sf_ref_param_read(r1, x),
          ref_ref_param_read(r2, x));
    check("ref_param_read referent", r1, r2);

    // 3. store through a reference local
    double l1 = 100.0, l2 = 100.0;
    check("ref_local ret", sf_ref_local(l1, x), ref_ref_local(l2, x));
    check("ref_local referent", l1, l2);

    // 4. compound store
    double c1 = 100.0, c2 = 100.0;
    check("ref_compound ret", sf_ref_compound(c1, x), ref_ref_compound(c2, x));
    check("ref_compound referent", c1, c2);

    // 5. integer reference (not a FLOP)
    int i1 = 100, i2 = 100;
    int ix = static_cast<int>(x);
    checkInt("ref_int ret", sf_ref_int(i1, ix), ref_ref_int(i2, ix));
    checkInt("ref_int referent", i1, i2);
  }

  std::printf(failures == 0 ? "\nALL REF-PARAM-STORE CHECKS PASSED\n"
                            : "\n%d REF-PARAM-STORE CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
