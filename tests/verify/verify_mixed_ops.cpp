// Differential check that the optimized unary / cast cases preserve semantics.
//
// Every expectation here is what the *source* expression tree computes, written
// out by hand. A node that got shared with a different operation shows up as a
// mismatch immediately; no FLOP count can see this difference at all.
#include <cmath>
#include <cstdio>

#include "mixed_ops.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-26s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. `-x` and `--x`: a = -x; x becomes x-1; b = x-1. The sum is -1 for any x.
  check("neg_vs_predec(1)", neg_vs_predec(1.0), -1.0);
  check("neg_vs_predec(5.5)", neg_vs_predec(5.5), -1.0);
  check("neg_vs_predec(-3.25)", neg_vs_predec(-3.25), -1.0);

  // 2. `(int)x` truncates towards zero and `(float)x` does not.
  check("casts(2.5)", cast_int_vs_float(2.5), 2.0 + 2.5);
  check("casts(5.5)", cast_int_vs_float(5.5), 10.5);
  check("casts(-3.25)", cast_int_vs_float(-3.25), -6.25);
  check("casts(0.5)", cast_int_vs_float(0.5), 0.5);

  // 3. The same cast twice must still be shared -- and still be one cast.
  check("same_cast(2.5)", cast_same_twice(2.5), 4.0);
  check("same_cast(5.5)", cast_same_twice(5.5), 10.0);
  check("same_cast(-3.25)", cast_same_twice(-3.25), -6.0);

  // 4. `-x` and `-(-x)` are different nodes: they sum to 0.
  check("neg_nested(1)", neg_and_inner_neg(1.0), 0.0);
  check("neg_nested(5.5)", neg_and_inner_neg(5.5), 0.0);
  check("neg_nested(-3.25)", neg_and_inner_neg(-3.25), 0.0);

  // 5. Value propagation copies a trivial initializer to its uses. The copy sits
  //    inside a cast, so the cast has to be rebuilt with the substituted
  //    operand; falling through to "return the node unchanged" left the use
  //    pointing at a declaration that was then deleted, i.e. the generated
  //    function did not even compile.
  check("cast_of_inlined(2.5)", cast_of_inlined_source(2.5), 2.0 + 2.5);
  check("cast_of_inlined(5.5)", cast_of_inlined_source(5.5), 10.5);
  check("cast_of_inlined(-3.25)", cast_of_inlined_source(-3.25), -6.25);

  std::printf(failures == 0 ? "\nALL MIXED-OP CHECKS PASSED\n"
                            : "\n%d MIXED-OP CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
