// Floating-point identities that hold only when the operands avoid 0 / +-inf /
// NaN, or when a signed zero can be ignored. The default profile licenses them
// through allowUnsafeFpIdentities; the conservative profile has to leave every
// expression alone, which is what this fixture pins. It runs in the `-s` stage
// only.
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
