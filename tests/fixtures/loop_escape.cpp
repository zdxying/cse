// Unrolling a loop deletes the loop *and* the counter it declares, so a read of
// that counter from the enclosing code would name nothing.
//
// The escape case cannot live in this file: its input (`return s + i;` after a
// `for (int i = ...)`) is not valid C++, so every verifier that compiles the
// output would reject the *fixture*. It is exercised as a shape check in
// run_tests.sh instead, on a generated input, where the assertion is "the loop
// survived" rather than "the file compiles".
//
// What is testable here are the controls: loops whose counters do *not* escape
// must still unroll (that is the pass's whole purpose), including the case where
// the loop counter shadows a parameter -- a different variable, so reading it
// after the loop is fine.

// Control: a contained counter unrolls. Four iterations of `s + a` collapse.
//@cse
double sf_contained_counter(double a) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s = s + a;
    }
    return s;
}
double ref_contained_counter(double a) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s = s + a;
    }
    return s;
}

// Control: the counter shadows a *parameter* of the same name. The parameter is
// a different variable, so reading it after the loop is fine and the loop still
// unrolls.
//@cse
double sf_param_shadow(double a, double i) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s = s + a;
    }
    return s + i;
}
double ref_param_shadow(double a, double i) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s = s + a;
    }
    return s + i;
}

// Control: two sibling loops that each declare `i`. Both counters are confined
// to their own loop, so both unroll -- and the second must not be renamed just
// because the first one declared the same spelling.
//@cse
double sf_sibling_counters(double a) {
    double s = 0.0;
    for (int i = 0; i < 2; ++i) {
        s = s + a;
    }
    for (int i = 0; i < 3; ++i) {
        s = s + a * 2.0;
    }
    return s;
}
double ref_sibling_counters(double a) {
    double s = 0.0;
    for (int i = 0; i < 2; ++i) {
        s = s + a;
    }
    for (int i = 0; i < 3; ++i) {
        s = s + a * 2.0;
    }
    return s;
}
