// A hoisting rewrite may not make a computation run where the source did not
// run it, when that computation can trap.
//
// `k ? 1/k : 0` evaluates the division only when k is non-zero. CSE used to see
// `1/k` twice (once per conditional), extract it to the top of the block, and
// turn the whole function into an unconditional `1/k` followed by two selects.
// At k = 0 the original returns 0; the optimized code traps (SIGFPE for integer
// division, inf for floating point).
//
// The same applies to `&&` / `||`: the right operand is short-circuited, so
// hoisting it runs it unconditionally.
//
// `sf_` and `ref_` twins share a body, so the twin differential compares the
// optimized value against the untouched one. The k = 0 input is the one that
// used to crash; it is in the edge-value stream the driver sweeps.

// `spec_div_ternary`: the guarded operand is a division by the condition.
//@cse
int sf_div_ternary(int k) {
    int x = k ? (10 / k) : 0;
    int y = k ? (10 / k) : 0;
    return x + y;
}
int ref_div_ternary(int k) {
    int x = k ? (10 / k) : 0;
    int y = k ? (10 / k) : 0;
    return x + y;
}

// `spec_div_and`: the same shape through a short-circuit `&&`. `k && (10/k)` is
// 1 when k > 0 (both operands non-zero) and 0 when k == 0, without evaluating
// the division.
//@cse
int sf_div_and(int k) {
    int x = k && (10 / k);
    int y = k && (10 / k);
    return x + y;
}
int ref_div_and(int k) {
    int x = k && (10 / k);
    int y = k && (10 / k);
    return x + y;
}

// `spec_float_div`: floating division does not trap, but hoisting it is still a
// change of *when* the operation happens -- keep the control case so the fix is
// not implemented by disabling extraction wholesale.
//@cse
double sf_float_div(double a, int k) {
    double x = k ? (a / k) : 0.0;
    double y = k ? (a / k) : 0.0;
    return x + y;
}
double ref_float_div(double a, int k) {
    double x = k ? (a / k) : 0.0;
    double y = k ? (a / k) : 0.0;
    return x + y;
}

// `spec_pure_arm`: the guarded operand is an ordinary product. Hoisting it is
// value-preserving, so CSE may still extract it -- this is the control that
// keeps the fix from being "never extract out of a conditional".
//@cse
double sf_pure_arm(double a, double b, int k) {
    double x = k ? (a * b) : 0.0;
    double y = k ? (a * b) : 0.0;
    return x + y;
}
double ref_pure_arm(double a, double b, int k) {
    double x = k ? (a * b) : 0.0;
    double y = k ? (a * b) : 0.0;
    return x + y;
}
