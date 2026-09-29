// Fixtures for cross-statement rewrites that must not outlive a store.
//
// The IR interns variables by name, so reassigning one does not create a new
// DAG node: two textually identical subexpressions stay the same node even
// though they compute different values. Both CSEPass (hoisting a shared
// subexpression) and ValueProp (copying an initializer to its use) used to
// ignore that, and both then produced silently wrong results.

extern int g_calls;

double side_effect(double x) {
  g_calls += 1;
  return x + (double)g_calls;
}

// Extracting here would make the second use read the value from before `a = 5`.
//@cse
double cse_across_store(double a, double x) {
  double p = a * x;
  a = 5.0;
  double q = a * x;
  return p + q;
}

// The same subexpression, but nothing is written in between: extraction is valid
// and must still happen.
//@cse
double cse_before_store(double a, double x) {
  double p = a * x;
  double q = a * x;
  a = 5.0;
  return p + q + a;
}

// `t` is never reassigned, so it used to be inlined as `x` -- which had already
// been overwritten by the time `t` was read.
//@cse
double vp_across_store(double a, double b) {
  double x = a;
  double t = x;
  x = b;
  return t;
}

// The same hazard with a parameter as the source of the copied value.
//@cse
double vp_param_store(double a, double b) {
  double t = a;
  a = b;
  return t;
}

// `const T*` only promises the pointee is not written through *this* pointer. If
// the caller passes the same buffer for both parameters, sharing `p[0]` across
// the store to `q[0]` changes the result.
//@cse
double const_alias(const double* p, double* q) {
  double s = p[0] + p[1];
  q[0] = 9.0;
  double t = p[0] + p[1];
  return s + t;
}

// A genuinely read-only source must keep being shared.
//@cse
double read_only_sum(const double* p) {
  double s = p[0] + p[1];
  double t = p[0] + p[1];
  return s + t;
}

// An impure call in between must not be collapsed either.
//@cse
double call_between(double a, double x) {
  double p = a * x;
  side_effect(1.0);
  double q = a * x;
  return p + q;
}
