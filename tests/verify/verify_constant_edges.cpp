// Constants at the edges: a division by zero, the sign of a zero literal, and a
// subscript that shares its node with a `1.0` written elsewhere.
//
// All three were wrong silently -- the emitted code either computed a different
// number, or (for the subscript) stopped compiling.
#include <cmath>
#include <cstdio>

#include "constant_edges.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-30s got=%.6g want=%.6g  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

static void check_true(const char* name, bool got) {
  if (!got) failures++;
  std::printf("%-30s %s\n", name, got ? "OK" : "FAIL");
}

int main() {
  // 1. `1.0 / 0.0` must survive as a division. The result is +inf -- not the 0
  //    the previous fold produced.
  const double inf = divide_by_zero_constant();
  check_true("divide_by_zero is +inf", std::isinf(inf) && inf > 0.0);

  // 2. `-0.0` is not `+0.0`. `1.0 * -0.0` is -0.0 (the sign is the xor of the
  //    two), and dropping the literal's sign turns it into +0.0.
  check_true("neg_zero(1) is negative", std::signbit(negative_zero_factor(1.0)));
  check_true("neg_zero(-1) is positive", !std::signbit(negative_zero_factor(-1.0)));
  check_true("1/neg_zero(1) is -inf",
             std::signbit(1.0 / negative_zero_factor(1.0)));

  // 3. Literal spellings that were already fine, pinned so the constant path
  //    cannot regress while it is being changed.
  check("exponent_literal(2)", exponent_literal(2.0), 2e16);
  check("exponent_literal(-0.5)", exponent_literal(-0.5), -5e15);
  check("fraction_literal(4)", fraction_literal(4.0), 3.0);
  check("fraction_literal(0)", fraction_literal(0.0), 1.0);

  // 4. The subscript shares its constant node with the `1.0` above it. If the
  //    emitter printed the source spelling there, this file would not compile at
  //    all (`feq[1.0]`), so reaching this line is most of the check.
  double feq[3] = {0.0, 0.0, 0.0};
  const double w = store_index_shares_literal(feq, 2.0);
  check("shared-literal subscript value", w, 2.0);
  check("shared-literal subscript store", feq[1], 2.0);

  std::printf(failures == 0 ? "\nALL CONSTANT-EDGE CHECKS PASSED\n"
                            : "\n%d CONSTANT-EDGE CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
