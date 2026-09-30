// An algebraic rewrite may not duplicate a side-effecting operand.
//
// `x * 2 -> x + x` is exact in IEEE-754 for values, which is why it needs no
// opt-in. Written out it is not: `2 * f()` calls f once, `f() + f()` calls it
// twice, and `2 * ++y` increments y once where `++y + ++y` increments it twice.
// The rewrite has to check that the operand it is about to repeat can be
// evaluated twice.
//
// `double_plain` and `double_shared_subexpr` are the controls: the rewrite has
// to keep firing for operands that may be repeated.

// Supplied by the verifier. Not in the FreeLB purity hook's list, so the call is
// impure and must not be duplicated.
double count_call(double x);

//@cse
double double_call(double x) {
    return 2.0 * count_call(x);
}
//@cse
double double_inc(double y) {
    double z = 2.0 * (++y);
    return z + y * 100.0;
}
//@cse
double double_plain(double x) {
    return 2.0 * x;
}
//@cse
double double_shared_subexpr(double a, double b) {
    double p = a * b;
    return 2.0 * p;
}
//@cse
