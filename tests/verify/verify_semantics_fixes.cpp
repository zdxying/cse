// Differential check that the semantic-safety fixes preserve behavior.
//
// tests/fixtures/semantics_fixes.cpp holds, for each case, a `//@cse`-marked
// function and an unmarked `ref_` twin with the same body. This file includes
// the optimized output and compares the two over many inputs -- no FLOP count
// can see a wrong operator, a dropped store or a reordered read.
#include <cmath>
#include <cstdio>

#include "semantics_fixes.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

static void checki(const char* name, int got, int want) {
  bool ok = got == want;
  if (!ok) failures++;
  std::printf("%-24s got=%d want=%d  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  const int ints[] = {-2, 0, 1, 2, 3};
  for (int a : ints)
    for (int b : ints)
      for (int c : ints) {
        checki("logic", sf_logic(a, b, c), ref_logic(a, b, c));
        checki("prec", sf_prec(a, b, c), ref_prec(a, b, c));
      }

  const double xs[] = {-3.25, -0.0, 0.0, 1.0, 2.5, 5.5};
  for (double x : xs) {
    check("unary", sf_unary(x), ref_unary(x));
    check("neg_dec", sf_neg_dec(x), ref_neg_dec(x));
    check("inc_zero", sf_inc_zero(x), ref_inc_zero(x));
    check("byref", sf_byref(x, x + 1.0), ref_byref(x, x + 1.0));
    check("vp_call", sf_vp_call(x), ref_vp_call(x));
    check("sibling", sf_sibling(x), ref_sibling(x));
    check("chain", sf_chain(x, x + 2.0), ref_chain(x, x + 2.0));
    check("tern", sf_tern(x, x + 1.0, x + 2.0), ref_tern(x, x + 1.0, x + 2.0));
    check("cast", sf_cast(x, x + 1.0), ref_cast(x, x + 1.0));
  }

  check("int_div", sf_int_div(), ref_int_div());
  checki("octal", sf_octal(), ref_octal());
  check("frac_for", sf_frac_for(), ref_frac_for());

  // Second batch (cases 19-25). Case 25 (`sf_lt_gt`) is compared here too, but
  // a passed-through region is still correct, so its real regression guard is
  // the shape check in run_tests.sh; the rest are caught by these values or by
  // failing to compile.
  {
    // A `for` init that is an assignment must set the induction variable: the
    // incoming `start` is deliberately not 0, so a dropped init is visible.
    const int starts[] = {0, 1, 5, 7};
    for (int st : starts)
      check("for_init", sf_for_init(2.5, st), ref_for_init(2.5, st));
  }
  for (double x : xs) {
    check("body_inc", sf_body_inc(x), ref_body_inc(x));
    check("local_inc", sf_local_inc(x), ref_local_inc(x));
    check("break", sf_break(x), ref_break(x));
    check("continue", sf_continue(x), ref_continue(x));
  }
  for (double x : xs) {
    check("neg_const", sf_neg_const(x, x + 1.5), ref_neg_const(x, x + 1.5));
    check("lt_gt", sf_lt_gt(x, x + 1.5), ref_lt_gt(x, x + 1.5));
  }

  // The base of a subscript must be parenthesized: `(p + 1)[i]`.
  {
    double buf[4] = {10.0, 11.0, 12.0, 13.0};
    check("postfix_base", sf_postfix_base(buf, 2), ref_postfix_base(buf, 2));
  }

  // Compound stores: compare the returned value and the stored element.
  for (double x : xs) {
    double p1[1] = {5.0}, p2[1] = {5.0};
    check("cmp_elem", sf_cmp_elem(p1, x), ref_cmp_elem(p2, x));
    if (p1[0] != p2[0]) {
      failures++;
      std::printf("cmp_elem store      got=%.6f want=%.6f  FAIL\n", p1[0], p2[0]);
    }
    SFBox b1{5.0}, b2{5.0};
    check("cmp_member", sf_cmp_member(b1, x), ref_cmp_member(b2, x));
  }

  // The impure call must survive `* 0.0` and still be made exactly once.
  {
    g_calls = 0.0;
    double v1 = sf_mul_zero(2.0);
    double c1 = g_calls;
    g_calls = 0.0;
    double v2 = ref_mul_zero(2.0);
    double c2 = g_calls;
    check("mul_zero", v1, v2);
    if (c1 != c2) {
      failures++;
      std::printf("mul_zero calls      got=%.0f want=%.0f  FAIL\n", c1, c2);
    } else {
      std::printf("%-24s got=%.0f want=%.0f  OK\n", "mul_zero calls", c1, c2);
    }
  }

  // `x + 0.0` at a negative zero is *not* compared here: the default profile
  // licenses `x + 0.0 -> x` under allowUnsafeFpIdentities, which turns -0.0 into
  // -0.0 where the source produced +0.0. That deviation is the licensed
  // trade-off, so the conservative contract is checked in
  // verify_float_identities.cpp (`-s`) instead.

  std::printf(failures == 0 ? "\nALL SEMANTICS-FIX CHECKS PASSED\n"
                            : "\n%d SEMANTICS-FIX CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
