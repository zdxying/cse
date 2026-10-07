// Regression fixtures for the semantic-safety fixes.
//
// Each `//@cse`-marked function has an unmarked `ref_` twin with the same body.
// The region extractor optimizes the marked ones and copies the twins through
// unchanged, so tests/verify/verify_semantics_fixes.cpp can compare the two
// numerically over many inputs. The cases cover: logical operators printed as
// bitwise ones, missing codegen parentheses (comparison / ternary / cast /
// nested unary), compound stores to an element or member, a by-reference
// argument written across a call, value propagation across an impure call, an
// impure call or increment discarded by `* 0.0`, a decrement deleted inside a
// double negation, integer division folded as floating point, octal literals,
// sibling-scope shadowing, chained-add merging that reads its target, and a
// fractional loop start. A second batch (cases 19-25, added later) covers a
// `for` init that is an assignment, a loop variable or body-local written by
// `++` inside the loop, a `break`/`continue` in an unrollable body, prefix `-`
// over a folded negative constant, and a `<`-then-`>` region that the template
// scanner used to mis-parse.
//
// Signed zero used to be pinned here, but the default profile licenses
// `x + 0.0 -> x` / `0.0 - x -> -x` under allowUnsafeFpIdentities (see
// tests/fixtures/float_identities.cpp), so a `sf_zero` vs `ref_zero` comparison
// would be checking the *unsafe* profile for a property it deliberately trades
// away. The conservative contract lives in verify_float_identities.cpp instead.

// --- shared helpers (outside every region) ---
double g_calls = 0.0;
double count_call(double x) {
  g_calls += 1.0;
  return x;
}
void mutate(double& r) { r += 10.0; }

struct SFBox {
  double v;
};

// 1. logical operators must not print as bitwise ones
//@cse
int sf_logic(int a, int b, int c) { return (a && b) + (a || c); }
int ref_logic(int a, int b, int c) { return (a && b) + (a || c); }

// 2. comparison precedence
//@cse
int sf_prec(int a, int b, int c) { return (a == b) < c; }
int ref_prec(int a, int b, int c) { return (a == b) < c; }

// 3. ternary parenthesization
//@cse
double sf_tern(double a, double b, double d) { return ((a > b) ? a : b) + d; }
double ref_tern(double a, double b, double d) { return ((a > b) ? a : b) + d; }

// 4. cast parenthesization
//@cse
double sf_cast(double a, double b) { return (double)(a + b); }
double ref_cast(double a, double b) { return (double)(a + b); }

// 5. nested unary operators
//@cse
double sf_unary(double x) { return +(+x) + -(-x); }
double ref_unary(double x) { return +(+x) + -(-x); }

// 6. compound store to an element
//@cse
double sf_cmp_elem(double* p, double x) { p[0] += x; return p[0]; }
double ref_cmp_elem(double* p, double x) { p[0] += x; return p[0]; }

// 7. compound store to a member
//@cse
double sf_cmp_member(SFBox b, double x) { b.v *= x; return b.v; }
double ref_cmp_member(SFBox b, double x) { b.v *= x; return b.v; }

// 8. by-reference argument written across a call
//@cse
double sf_byref(double a, double b) { double x = a + b; mutate(b); double y = a + b; return x + y; }
double ref_byref(double a, double b) { double x = a + b; mutate(b); double y = a + b; return x + y; }

// 9. value propagation across an impure call
//@cse
double sf_vp_call(double a) { double t = a; mutate(a); return t * 2.0; }
double ref_vp_call(double a) { double t = a; mutate(a); return t * 2.0; }

// 10. impure call must survive `* 0.0`
//@cse
double sf_mul_zero(double x) { return count_call(x) * 0.0; }
double ref_mul_zero(double x) { return count_call(x) * 0.0; }

// 11. increment must survive `* 0.0`
//@cse
double sf_inc_zero(double y) { double z = (++y) * 0.0; return z + y; }
double ref_inc_zero(double y) { double z = (++y) * 0.0; return z + y; }

// 12. decrement inside a double negation
//@cse
double sf_neg_dec(double x) { return -(--x); }
double ref_neg_dec(double x) { return -(--x); }

// 13. integer division must not be folded as floating point
//@cse
double sf_int_div() { double d = 7 / 2; return d; }
double ref_int_div() { double d = 7 / 2; return d; }

// 14. octal literal
//@cse
int sf_octal() { return 010 == 10; }
int ref_octal() { return 010 == 10; }

// 15. sibling-scope shadowing
//@cse
double sf_sibling(double a) {
  double x = 0.0;
  { double t = a; x = t + 1.0; }
  double y = 0.0;
  { double t = 2.0; y = t + 1.0; }
  return x + y;
}
double ref_sibling(double a) {
  double x = 0.0;
  { double t = a; x = t + 1.0; }
  double y = 0.0;
  { double t = 2.0; y = t + 1.0; }
  return x + y;
}

// 16. chained-add merge must not read the target
//@cse
double sf_chain(double a, double b) { double x = 0.0; x = x + a; x = x + x * b; return x; }
double ref_chain(double a, double b) { double x = 0.0; x = x + a; x = x + x * b; return x; }

// 17. a loop with a fractional start must not be unrolled
//@cse
double sf_frac_for() { double s = 0; for (double i = 0.5; i < 3; ++i) { s += i; } return s; }
double ref_frac_for() { double s = 0; for (double i = 0.5; i < 3; ++i) { s += i; } return s; }

// 18. the base of a subscript must be parenthesized
//@cse
double sf_postfix_base(double* p, int i) { return (p + 1)[i]; }
double ref_postfix_base(double* p, int i) { return (p + 1)[i]; }

// --- second batch: loop-unroll, for-init, unary emission, `<` disambiguation ---
// These six were found in a later review and all predate the fixes above. Each
// is value-comparable except case 25, which is a "region was dropped" defect:
// a passed-through region is still correct, so the verifier cannot see it and
// run_tests.sh checks its shape instead.

// 19. a `for` init that is an assignment, not a declaration, must be emitted.
// Dropping it left `for (; start < 3; ++start)`, so the induction variable kept
// its incoming value.
//@cse
double sf_for_init(double a, int start) {
  double s = 0.0;
  for (start = 0; start < 3; ++start) { s += a; }
  return s * 10.0 + start;
}
double ref_for_init(double a, int start) {
  double s = 0.0;
  for (start = 0; start < 3; ++start) { s += a; }
  return s * 10.0 + start;
}

// 20. a loop whose own variable is modified by `++` in the body must not be
// unrolled: substituting the constant into `i++` produced `0.0++`.
//@cse
double sf_body_inc(double a) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    s += a;
    i++;
  }
  return s;
}
double ref_body_inc(double a) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    s += a;
    i++;
  }
  return s;
}

// 21. a body-local modified by `++` must be renamed, not inlined: inlining
// produced `(a + 1.0)++`.
//@cse
double sf_local_inc(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + 1.0;
    t++;
    s += t;
  }
  return s;
}
double ref_local_inc(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + 1.0;
    t++;
    s += t;
  }
  return s;
}

// 22. a `break` in the body must block unrolling: splicing the body into the
// parent block left a top-level `break;`. (`>=` rather than `>` on purpose, so
// the case does not also depend on the `<`-disambiguation fix below.)
//@cse
double sf_break(double a) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (a >= 1.0) break;
    s += a;
  }
  return s;
}
double ref_break(double a) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (a >= 1.0) break;
    s += a;
  }
  return s;
}

// 23. ... and likewise a `continue`.
//@cse
double sf_continue(double a) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (a >= 1.0) continue;
    s += a;
  }
  return s;
}
double ref_continue(double a) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (a >= 1.0) continue;
    s += a;
  }
  return s;
}

// 24. prefix `-` over a folded negative constant must not fuse into `--5`. The
// reassociator pulls the (negative) constant to the front of the sum.
//@cse
double sf_neg_const(double a, double b) {
  double p = a - (2.0 - 7.0);
  double q = b - (2.0 - 7.0);
  return p + q;
}
double ref_neg_const(double a, double b) {
  double p = a - (2.0 - 7.0);
  double q = b - (2.0 - 7.0);
  return p + q;
}

// 25. a `<` comparison followed by a `>` later in the region must parse. The
// old template scan swallowed everything up to the first `>` as one
// "template-id", so the whole region failed to parse and was passed through.
//@cse
double sf_lt_gt(double a, double b) {
  double x = a * b;
  double y = a * b;
  if (a < b) { x = x + 1.0; }
  if (a > b) { y = y + 1.0; }
  return x + y;
}
double ref_lt_gt(double a, double b) {
  double x = a * b;
  double y = a * b;
  if (a < b) { x = x + 1.0; }
  if (a > b) { y = y + 1.0; }
  return x + y;
}
