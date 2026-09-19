// Differential check that the optimized safety cases preserve semantics.
#include <cmath>
#include <cstdio>

#include "safety_cases.cpp.cse"

int g_calls = 0;

static int failures = 0;
static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-14s got=%.6f want=%.6f  %s\n", name, got, want,
              ok ? "OK" : "FAIL");
}

int main() {
  // 1. Impure calls must run twice (side effect observed).
  g_calls = 0;
  double a = impure_calls(2.5);
  g_calls = 0;
  double wantA = side_effect(2.5) + side_effect(2.5);
  check("impure_calls", a, wantA);

  // 2. Load must not be reused across a store.
  double arr1[3] = {1.0, 2.0, 3.0};
  double arr2[3] = {1.0, 2.0, 3.0};
  double m = load_store(arr1, 1);
  double t = arr2[1];
  arr2[1] = t + 1.0;
  double z = arr2[1];
  double wantM = z + t;
  check("load_store", m, wantM);
  bool sameArray = (arr1[1] == arr2[1]) && (arr1[0] == arr2[0]);
  std::printf("%-14s %s\n", "store effect", sameArray ? "OK" : "FAIL");
  if (!sameArray) failures++;

  // 3. Branch-local subexpression must not be hoisted at the expense of
  //    semantics.
  double bh1 = branch_hoist(2.0, 3.0, 1);
  double wantBh1 = (2.0 * 3.0 + 2.0 * 3.0) + 2.0 * 3.0;
  check("branch(true)", bh1, wantBh1);
  double bh0 = branch_hoist(2.0, 3.0, 0);
  double wantBh0 = 0.0 + 2.0 * 3.0;
  check("branch(false)", bh0, wantBh0);

  // 4. Comparison operators must round-trip through codegen.
  //    a=3,b=5 -> (==)0 + (<=)2 + (!=)4 + (>=)0 = 6
  check("comparisons", comparisons(3, 5), 6.0);
  //    a=5,b=5 -> 1 + 2 + 0 + 8 = 11
  check("comparisons eq", comparisons(5, 5), 11.0);

  // 5. Shadowing: inner y must not clobber the outer y.
  check("shadowing", shadowing(2.0), 3.0);

  std::printf(failures == 0 ? "\nALL SAFETY CHECKS PASSED\n"
                            : "\n%d SAFETY CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
