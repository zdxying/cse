// Differential check that an impure operand is not duplicated by the
// `x * 2 -> x + x` rewrite, and that the rewrite still fires when it may.
#include <cmath>
#include <cstdio>

#include "effect_duplication.cpp.cse"

int g_calls = 0;

double count_call(double x) {
  g_calls += 1;
  return x;
}

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-22s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

static void checkCalls(const char* name, int got, int want) {
  bool ok = got == want;
  if (!ok) failures++;
  std::printf("%-22s calls=%d want=%d  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. `2 * count_call(x)` must call it once.
  g_calls = 0;
  check("double_call(3)", double_call(3.0), 6.0);
  checkCalls("double_call", g_calls, 1);

  // 2. `2 * ++y`: y is incremented once, so the result is
  //    z + 100y = 2(y+1) + 100(y+1) = 102(y+1).
  check("double_inc(1)", double_inc(1.0), 204.0);
  check("double_inc(2.5)", double_inc(2.5), 102.0 * 3.5);

  // 3. Controls: a plain variable and a pure subexpression may still be doubled.
  check("double_plain(3)", double_plain(3.0), 6.0);
  check("double_plain(-1.5)", double_plain(-1.5), -3.0);
  check("double_subexpr(2,3)", double_shared_subexpr(2.0, 3.0), 12.0);

  std::printf(failures == 0 ? "\nALL EFFECT-DUPLICATION CHECKS PASSED\n"
                            : "\n%d EFFECT-DUPLICATION CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
