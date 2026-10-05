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
// fractional loop start.
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
