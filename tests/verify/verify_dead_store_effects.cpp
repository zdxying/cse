// DCE must keep a store whose value carries an effect, and must still delete one
// that carries none.
//
// The increment/decrement cases are the ones that were wrong: the store was
// deleted and the increment went with it, so the function returned `a` instead
// of `a + 1` -- no diagnostic, no crash, just a different number.
#include <cmath>
#include <cstdio>

#include "dead_store_effects.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-28s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. `unused = y++;` increments `y` even though `unused` is never read.
  check("inc_in_dead_store(1)", inc_in_dead_store(1.0), 2.0);
  check("inc_in_dead_store(-3.5)", inc_in_dead_store(-3.5), -2.5);
  check("inc_in_dead_store(0)", inc_in_dead_store(0.0), 1.0);

  // 2. `unused = --y;` decrements `y`.
  check("dec_in_dead_store(5)", dec_in_dead_store(5.0), 4.0);
  check("dec_in_dead_store(-1)", dec_in_dead_store(-1.0), -2.0);

  // 3. A store to a name this function does not declare may be a global, and is
  //    visible from outside regardless of what the function reads back.
  g_written = 0.0;
  write_through_global(7.0);
  check("g_written after call", g_written, 7.0);

  // 4. An impure call is an effect. This one already worked; it is here so that
  //    widening the test cannot silently lose it.
  g_calls = 0.0;
  call_in_dead_store(3.0);
  check("g_calls after call", g_calls, 1.0);

  // 5. A dead store of a pure value: the value is unchanged, and this is the
  //    store DCE is both allowed and expected to delete (the shape check in
  //    run_tests.sh checks that it still does).
  check("dead_pure_store(2,3)", dead_pure_store(2.0, 3.0), 5.0);
  check("dead_pure_store(-4,0.25)", dead_pure_store(-4.0, 0.25), -3.75);

  std::printf(failures == 0 ? "\nALL DEAD-STORE CHECKS PASSED\n"
                            : "\n%d DEAD-STORE CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
