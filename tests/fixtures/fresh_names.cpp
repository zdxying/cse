// A name a pass *invents* must not collide with a name the source uses.
//
// Variables are interned by name (IRModule::getVar), so two declarations that
// end up with the same internal name are the *same* DAG node. Every generator
// that concatenated a suffix and hoped was therefore a silent miscompile
// waiting for a user identifier that happened to match:
//
//   * CSE materializes a shared expression as `_cse_<iter>_<id>`. A user
//     variable spelled `_cse_0_4` became that same node, and value propagation
//     then inlined the user's value into every use of the shared expression.
//   * Loop unrolling renames a mutable body local to `t__u<i>`. A parameter
//     spelled `t__u0` collided with the first iteration's local -- a
//     declaration that shadows a parameter (or, where that compiles, silently
//     rebinds it).
//   * The builder alpha-renames a shadowed local to `x__s<n>`.
//
// `IRModule::freshName` is now the one authority. The fixtures below spell user
// identifiers in exactly those shapes; each `sf_`/`ref_` pair must agree.

// A user variable in CSE's temporary namespace. `sf_cse_name` shares `a * b`
// twice, so CSE wants a temporary -- and the user already owns `_cse_0_4`.
//@cse
double sf_cse_name(double a, double b, double c) {
    double _cse_0_4 = c;
    double t = a * b;
    double u = a * b;
    return t + u + _cse_0_4;
}
double ref_cse_name(double a, double b, double c) {
    double _cse_0_4 = c;
    double t = a * b;
    double u = a * b;
    return t + u + _cse_0_4;
}

// A parameter in the unroller's rename namespace. The body local `t` is
// reassigned, so the unroller must rename it per iteration -- to something that
// is not the caller's `t__u0`.
//@cse
double sf_unroll_name(double a, double t__u0) {
    double s = 0.0;
    for (int i = 0; i < 2; ++i) {
        double t = a * 2.0;
        t = t + a;
        s = s + t;
    }
    return s + t__u0;
}
double ref_unroll_name(double a, double t__u0) {
    double s = 0.0;
    for (int i = 0; i < 2; ++i) {
        double t = a * 2.0;
        t = t + a;
        s = s + t;
    }
    return s + t__u0;
}

// A user variable in the builder's shadow-rename namespace. The inner `x`
// shadows the outer one and must be renamed to something that is not the
// user's own `x__s1`.
//@cse
double sf_shadow_name(double a, double b, double x__s1) {
    double x = a;
    {
        double x = b;
        a = x * 2.0;
    }
    return x + a + x__s1;
}
double ref_shadow_name(double a, double b, double x__s1) {
    double x = a;
    {
        double x = b;
        a = x * 2.0;
    }
    return x + a + x__s1;
}
