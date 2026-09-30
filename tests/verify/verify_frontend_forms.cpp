// Differential check for the frontend forms a real kernel writes.
//
// Before these parsed, the only way to see the defect was to watch the driver
// abort; the check here is that the generated function still computes what the
// source says, for the postfix value semantics in particular (`y++` yields the
// old value, `++y` the new one).
#include <cmath>
#include <cstdio>

#include "frontend_forms.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. `i++` in a counted loop: four iterations, so `a` grows by 4*b.
  check("counted_postfix", counted_postfix(1.0, 2.0), 1.0 + 4.0 * 2.0);

  // 2. The same update with a non-constant bound stays a loop, and must still
  //    run exactly n times.
  check("postfix_loop(3)", postfix_loop(1.0, 2.0, 3), 7.0);
  check("postfix_loop(0)", postfix_loop(1.0, 2.0, 0), 1.0);

  // 3. `z = y++` binds the *old* value and leaves y at x+1: (x+1)*10 + x.
  check("postfix_value(2)", postfix_value(2.0), 32.0);
  check("postfix_value(-1.5)", postfix_value(-1.5), (-1.5 + 1.0) * 10.0 + (-1.5));

  // 4. A discarded postfix increment still has to increment.
  check("postfix_stmt(2)", postfix_stmt(2.0), 3.0);

  // 5. Unary `+` is the identity, and it shares its character with `++`.
  check("plus_vs_preinc(1)", plus_vs_preinc(1.0), 3.0);
  check("plus_vs_preinc(5.5)", plus_vs_preinc(5.5), 12.0);
  check("plus_vs_preinc(-3.25)", plus_vs_preinc(-3.25), -5.5);

  // 6. Scientific notation. The constant sum folds during optimization, so the
  //    emitted literal has to round-trip exactly.
  check("sci_notation", sci_notation(1.0), 1e16 + 6.02e23 + 1e-9 + 1.0);
  check("sci_upper(3)", sci_upper(3.0), 2.5e-3 * 3.0);

  std::printf(failures == 0 ? "\nALL FRONTEND-FORM CHECKS PASSED\n"
                            : "\n%d FRONTEND-FORM CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
