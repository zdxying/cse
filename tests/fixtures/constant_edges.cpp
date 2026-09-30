// Constants at the edges.
//
// Three independent ways the emitted constant could stop matching the source one:
//
//   * `foldConst` folded `a / 0` to 0, because the switch case read
//     `rhs != 0 ? a / rhs : 0`. Floating-point division by zero is infinity or
//     NaN, so `1.0 / 0.0` came out as `return 0;` -- a value nobody asked for and
//     nothing reported. The IR carries no types, so the expression stands now.
//   * `createConst` re-derived every constant's text from its `double` and
//     normalized integral values to integer digits. That turned the literal
//     `-0.0` into `-0`, which is an *integer* zero: it converts back to +0.0, so
//     `1.0 / -0.0` (+inf in the source) silently became -inf. The source spelling
//     is kept now.
//   * keeping the spelling then broke subscripts, because a constant's text is
//     not part of its identity: the `1.0` written in an expression and the `1`
//     written as a subscript are the *same node*, so the index was printed with
//     the floating spelling -- which does not compile. Subscripts are emitted as
//     integers now.
//
// tests/verify/verify_constant_edges.cpp checks the values, and run_tests.sh
// greps the emitted text, because the point of two of these is the spelling.

//@cse
double divide_by_zero_constant() {
    return 1.0 / 0.0;
}
//@cse
double negative_zero_factor(double x) {
    double y = -0.0;
    return x * y;
}
//@cse
double exponent_literal(double x) {
    double k = 1e16;
    return k * x;
}
//@cse
double fraction_literal(double x) {
    double h = 0.5;
    return h * x + 1.0;
}
//@cse
double store_index_shares_literal(double* feq, double x) {
    double w = 1.0 * x;
    feq[1] = w;
    return w;
}
//@cse
