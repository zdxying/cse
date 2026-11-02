// Exact algebraic identities: each of these rewrites is bit-identical for
// IEEE-754 (including +-0.0, +-inf, NaN and subnormals), so the conservative
// profile keeps them -- unlike the identities in float_identities.cpp, which
// need a licence because they change a special value. This fixture runs in the
// `-s` stage and pins that the exact tier is *not* disabled there.
//
// See tests/verify/verify_config.cpp for the library-level contract and
// tests/verify/verify_exact_identities.cpp for the numerical check.

//@cse
double mul_one(double x) { return x * 1.0; }
//@cse
//@cse
double one_mul(double x) { return 1.0 * x; }
//@cse
//@cse
double div_one(double x) { return x / 1.0; }
//@cse
//@cse
double sub_zero(double x) { return x - 0.0; }
//@cse
//@cse
double add_neg(double a, double b) { return a + (-b); }
//@cse
//@cse
double sub_neg(double a, double b) { return a - (-b); }
//@cse
//@cse
double double_neg(double x) { return -(-x); }
//@cse
//@cse
double twice(double x) { return x * 2.0; }
//@cse
//@cse
double even_square(double a) { return (-a) * (-a); }
//@cse
//@cse
double neg_mul(double a, double b) { return a * (-b); }
//@cse
