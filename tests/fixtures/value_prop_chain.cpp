// Regression fixtures for a value-propagation miscompile: a chain of trivial
// locals leaves a dangling reference.
//
// Each `//@cse`-marked function has an unmarked `ref_` twin with the same body.
// The region extractor optimizes the marked ones and copies the twins through
// unchanged, so tests/verify/verify_value_prop_chain.cpp can compare the two
// numerically.
//
// The value-propagation pass substitutes one definition per use site. Its
// `propExpr` resolves a single link, so when trivial definitions chain --
// `double u = t;` with `double t = a;` -- a use of `u` is rewritten to `t`, not
// straight to `a`. The pass then deleted every declaration it had collected,
// including `t`, whose rewritten body still mentioned it. The result referenced
// an undeclared name (a compile error), or -- when an outer/global of the same
// name existed -- silently bound to that other variable instead of the value.
//
// The cases below exercise a two-link chain, a three-link chain, a chain seeded
// by a shareable member load, a chained name used several times, and the
// silent-wrong-binding variant where the local shadows a global of the same
// name. The last one is the one no compiler error can catch: it compiles and
// runs, but reads the wrong variable.

struct VPCBox {
  double v;
};

// A global that the shadow case below re-declares locally. Dropping the local
// makes the emitted code bind this global instead.
double vpc_shadow = 7.0;

// 1. two-link chain: u -> t -> a
//@cse
double sf_chain2(double a) {
  double t = a;
  double u = t;
  return u * 2.0;
}
double ref_chain2(double a) {
  double t = a;
  double u = t;
  return u * 2.0;
}

// 2. three-link chain: w -> u -> t -> a
//@cse
double sf_chain3(double a) {
  double t = a;
  double u = t;
  double w = u;
  return w * 2.0;
}
double ref_chain3(double a) {
  double t = a;
  double u = t;
  double w = u;
  return w * 2.0;
}

// 3. chain seeded by a shareable member load: u -> t -> q.v
//@cse
double sf_chain_member(VPCBox q) {
  double t = q.v;
  double u = t;
  return u * 2.0;
}
double ref_chain_member(VPCBox q) {
  double t = q.v;
  double u = t;
  return u * 2.0;
}

// 4. the chained name is used more than once: every use must be rewritten
//    before its declaration may go.
//@cse
double sf_chain_expr(double a) {
  double t = a;
  double u = t;
  return u * u + u;
}
double ref_chain_expr(double a) {
  double t = a;
  double u = t;
  return u * u + u;
}

// 5. silent wrong binding: the chained local shadows a global of the same name.
//    A dangling reference would compile here (binding `vpc_shadow`), so only a
//    value comparison can see it.
//@cse
double sf_chain_shadow(double a) {
  double vpc_shadow = a;
  double u = vpc_shadow;
  return u * 2.0;
}
double ref_chain_shadow(double a) {
  double vpc_shadow = a;
  double u = vpc_shadow;
  return u * 2.0;
}
