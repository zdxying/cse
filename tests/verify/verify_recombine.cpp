// Differential check that the expression-recombination stage (`cse -r`)
// preserves semantics: the fixture is optimized with -r and re-executed here
// against the unoptimized formulation.
//
// The division cases are the point of this verifier. `a / x + b / x` must come
// out as two divisions: an earlier defect emitted `(a + b) * x`, and even the
// "obvious" `(a + b) / x` would change integer results. The impure case guards
// the other direction -- a side-effecting call must never collapse into one
// evaluation.
#include <cmath>
#include <cstdio>

#include "recombine.cpp.cse"

int g_calls = 0;

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-20s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // Factored products must stay numerically identical to the sum of products.
  check("factor_left", factor_common_left(2.0, 3.0, 4.5),
        2.0 * 3.0 + 2.0 * 4.5);
  check("factor_right", factor_common_right(2.0, 5.0, 3.0),
        2.0 * 3.0 + 5.0 * 3.0);
  check("factor_sub", factor_sub(7.0, 2.0, 3.0), 7.0 * 3.0 - 2.0 * 3.0);
  check("factor_cross", factor_cross(2.0, 3.0, 4.5), 2.0 * 3.0 + 4.5 * 2.0);
  check("factor_plus_one", factor_plus_one(2.0, 3.0), 2.0 * 3.0 + 2.0);
  check("factor_minus_one", factor_minus_one(7.0, 3.0), 7.0 * 3.0 - 7.0);

  // Division must remain two separate divisions. A value of 8.0 here means the
  // old `(a + b) * x` rewrite came back.
  double div = division_unfactored(3.0, 1.0, 2.0);
  check("division", div, 3.0 / 2.0 + 1.0 / 2.0);
  if (div != 2.0)
    std::printf("               (division was rewritten: expected 2.0)\n");

  // Integer division is why division is never factored:
  // 3/2 + 1/2 == 1, whereas (3+1)/2 == 2.
  check("int_division", int_division(3, 1, 2), 1.0);
  check("int_division 2", int_division(7, 7, 2), 6.0);

  // A side-effecting call must be evaluated once per occurrence: collapsing the
  // two calls into a shared factor both loses a side effect and changes the
  // value. The two evaluations observe g_calls == 1 and == 2, and the order in
  // which the surrounding products are evaluated is unspecified, so accept
  // either pairing.
  g_calls = 0;
  double imp = impure_factor(2.0, 3.0, 4.0);
  int calls = g_calls;
  double s1 = 4.0 + 1.0;
  double s2 = 4.0 + 2.0;
  bool valueOk = std::fabs(imp - (2.0 * s1 + 3.0 * s2)) < 1e-12 ||
                 std::fabs(imp - (2.0 * s2 + 3.0 * s1)) < 1e-12;
  bool callsOk = (calls == 2);
  std::printf("%-20s calls=%d want=2  %s\n", "impure calls", calls,
              callsOk ? "OK" : "FAIL");
  std::printf("%-20s got=%.6f want=%.6f (either order)  %s\n", "impure_factor",
              imp, 2.0 * s1 + 3.0 * s2, valueOk ? "OK" : "FAIL");
  if (!callsOk || !valueOk) failures++;

  std::printf(failures == 0 ? "\nALL RECOMBINE CHECKS PASSED\n"
                            : "\n%d RECOMBINE CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
