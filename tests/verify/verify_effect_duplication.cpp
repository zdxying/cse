// Differential check that an impure operand is not duplicated by the
// `x * 2 -> x + x` rewrite, and that the rewrite still fires when it may.
#include <cmath>
#include <cstdio>

#include "effect_duplication.cpp.cse"
#include "harness.h"

int g_calls = 0;

double count_call(double x) {
  g_calls += 1;
  return x;
}

int main() {
  // 1. `2 * count_call(x)` must call it once.
  g_calls = 0;
  check("double_call(3)", double_call(3.0), 6.0);
  checkInt("double_call", g_calls, 1);

  // 1b. The call hidden one level down must not be duplicated either:
  //     `count_call(x) + 1.0` is a BinaryOp with its own `pure == true`, so a
  //     shallow check missed it. 2*(3+1) = 8, one call.
  g_calls = 0;
  check("double_call_compound(3)", double_call_compound(3.0), 8.0);
  checkInt("double_call_compound", g_calls, 1);
  //     Control: a pure compound operand may still be doubled.
  check("double_pure_compound(3)", double_pure_compound(3.0), 8.0);

  // 2. `2 * ++y`: y is incremented once, so the result is
  //    z + 100y = 2(y+1) + 100(y+1) = 102(y+1).
  check("double_inc(1)", double_inc(1.0), 204.0);
  check("double_inc(2.5)", double_inc(2.5), 102.0 * 3.5);

  // 3. Controls: a plain variable and a pure subexpression may still be doubled.
  check("double_plain(3)", double_plain(3.0), 6.0);
  check("double_plain(-1.5)", double_plain(-1.5), -3.0);
  check("double_subexpr(2,3)", double_shared_subexpr(2.0, 3.0), 12.0);

  return finish("EFFECT-DUPLICATION");
}
