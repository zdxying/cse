// Differential check that the generated code preserves the parenthesisation of
// the source. Every case is recomputed here in the unoptimized formulation.
//
// Compile against the default-profile output by default, or against another
// generated header via -DCSE_INPUT='"<name>"' (run_tests.sh uses this for -s).
#include <cmath>
#include <cstdio>

#ifdef CSE_INPUT
#include CSE_INPUT
#else
#include "parens.cpp.cse"
#endif

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-16s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

static void checkInt(const char* name, int got, int want) {
  bool ok = (got == want);
  if (!ok) failures++;
  std::printf("%-16s got=%d want=%d  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  check("a-(b-c)", sub_sub(10, 3, 1), 10.0 - (3.0 - 1.0));
  check("a-(b+c)", sub_add(10, 3, 1), 10.0 - (3.0 + 1.0));
  check("a+(b-c)", add_sub(10, 3, 1), 10.0 + (3.0 - 1.0));
  check("a/(b*c)", div_mul(1, 2, 3), 1.0 / (2.0 * 3.0));
  check("a/(b/c)", div_div(1, 2, 3), 1.0 / (2.0 / 3.0));
  // Integer division is where dropping the parentheses on `a * (b / c)` is
  // plainly wrong: 2 * (3 / 2) is 2, while 2 * 3 / 2 is 3.
  checkInt("a*(b/c)", mul_div(2, 3, 2), 2 * (3 / 2));
  checkInt("a%(b%c)", mod_mod(10, 7, 4), 10 % (7 % 4));
  check("a-(b-(c-d))", nested_right(10, 3, 7, 2), 10.0 - (3.0 - (7.0 - 2.0)));
  // Left-nested chains round-trip without parentheses, and must keep doing so.
  check("a-b-c", left_chain(10, 3, 1), 10.0 - 3.0 - 1.0);
  check("(a-b)-c", grouped_left(10, 3, 1), (10.0 - 3.0) - 1.0);

  std::printf(failures == 0 ? "\nALL PARENS CHECKS PASSED\n"
                            : "\n%d PARENS CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
