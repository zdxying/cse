// Fixtures for the expression-recombination stage, optimized with `cse -r`
// (see tests/run_tests.sh). They pin three things at once:
//
//   * factoring still happens for multiplication -- a positive signal, since a
//     silently disabled pass would show up as the un-factored FLOP count;
//   * the `a*x ± a` forms are reachable at all (they used to be dead code,
//     because the handler returned early unless both operands of the +/- node
//     were binary expressions);
//   * nothing is rewritten for division, nor through a side-effecting call.
//
// Each pattern sits in its own single-expression function on purpose: within one
// function, a subexpression repeated across two statements would be extracted by
// CSEPass first, hiding the pattern from this pass.
//
// `a / x + b / x` is deliberately NOT rewritten: (a+b)/x is valid for exact
// division only -- with integers 3/2 + 1/2 == 1 while (3+1)/2 == 2 -- and the IR
// carries no type information to tell the two apart.

extern int g_calls;

double side_effect(double x) {
  g_calls += 1;
  return x + (double)g_calls;
}

//@cse
double factor_common_left(double a, double x, double y) {
  return a * x + a * y;
}

//@cse
double factor_common_right(double a, double b, double x) {
  return a * x + b * x;
}

//@cse
double factor_sub(double a, double b, double x) {
  return a * x - b * x;
}

// Cross combination: the shared factor is the second one on the left and the
// first one on the right, so extracting it reorders the products. Requires
// assumeNumericCommutative.
//@cse
double factor_cross(double a, double x, double y) {
  return a * x + y * a;
}

// The `a*x ± a` forms do not change the FLOP count, so the pinned total cannot
// detect their loss; run_tests.sh also greps the generated text for them.
//@cse
double factor_plus_one(double a, double x) {
  return a * x + a;
}

//@cse
double factor_minus_one(double a, double x) {
  return a * x - a;
}

//@cse
double division_unfactored(double a, double b, double x) {
  return a / x + b / x;
}

//@cse
double int_division(int a, int b, int x) {
  int r = a / x + b / x;
  return (double)r;
}

//@cse
double impure_factor(double a, double b, double x) {
  return a * side_effect(x) + b * side_effect(x);
}
