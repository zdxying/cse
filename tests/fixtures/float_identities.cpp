// Floating-point identities that hold only when the operands avoid 0 / +-inf /
// NaN, or when a signed zero can be ignored. The default profile licenses them
// through allowUnsafeFpIdentities; the conservative profile must leave these
// particular expressions alone, which is what this fixture pins. It runs in the
// `-s` stage only.
//
// These are the *licensed* identities. The bit-exact rewrites that `-s` keeps
// (x*1, x-0, x/1, x*2, ...) are pinned by exact_identities.cpp instead.
//
// See tests/verify/verify_config.cpp for the library-level contract, and
// tests/verify/verify_float_identities.cpp for the numerical check.

//@cse
double self_div(double x) { return x / x; }
//@cse
//@cse
double zero_over(double x) { return 0.0 / x; }
//@cse
//@cse
double self_sub(double x) { return x - x; }
//@cse
//@cse
double times_zero(double x) { return x * 0.0; }
//@cse
//@cse
double add_zero(double x) { return x + 0.0; }
//@cse
//@cse
double zero_minus(double x) { return 0.0 - x; }
//@cse
// The chained-additive merge (`y = 0; y = y + a; y = y + a` -> `y = a + a`)
// drops the leading zero, so it is a signed-zero identity too: at a = -0.0 the
// original yields (0.0 + -0.0) + -0.0 = +0.0 while the merged form yields
// -0.0 + -0.0 = -0.0. The conservative profile must keep the chain.
//@cse
double cleanup_chain(double a) {
    double y = 0.0;
    y = y + a;
    y = y + a;
    return y;
}
//@cse
