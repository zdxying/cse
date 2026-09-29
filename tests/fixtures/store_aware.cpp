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

// ---- Element / member stores -------------------------------------------
//
// An element or member store writes through the *root* of its lvalue, so it has
// to count as a write to that whole variable. Both scans used to miss it: the
// store is not a plain `x = v;` assignment, and in the default configuration it
// is not structural in the IR either (see IRBuilder), so it arrives as an
// ExprStmt wrapping an opaque `BinaryOp(=)`.

struct Pair {
  double a;
  double b;
};

// Supplied by the verifier. The names carry prefixes that the FreeLB profile
// purity hook accepts, which is what makes the call node interned -- and
// therefore a CSE candidate.
double getnorm_pair(Pair p);
double getsum_array(double* a);

// Copying `p.a` to the use site would read the value stored afterwards.
//@cse
double vp_member(Pair p, double v) {
  double t = p.a;
  p.a = v;
  return t;
}

//@cse
double vp_arrow(Pair* p, double v) {
  double t = p->a;
  p->a = v;
  return t;
}

// Sharing the call across the store would fold two reads of the object into
// one, taken before the store.
//@cse
double cse_member_call(Pair p) {
  double u = getnorm_pair(p);
  p.a = 5.0;
  double w = getnorm_pair(p);
  return u + w;
}

//@cse
double cse_element_call(double* a) {
  double u = getsum_array(a);
  a[0] = 5.0;
  double w = getsum_array(a);
  return u + w;
}

// A store to one object must not block sharing that concerns another, untouched
// one -- the check has to stay precise, not just conservative.
//@cse
double cse_other_object(Pair p, double x, double* q) {
  double u = p.a * x;
  q[0] = 5.0;
  double w = p.a * x;
  return u + w;
}

// ---- What the representation change has to preserve --------------------
//
// These two pin the parts of the lvalue slot that are easy to lose when a store
// stops being an opaque expression: the root is a *use* of the variable, and the
// index is an ordinary expression that still gets optimized.

// `p` is only ever written, never read -- the returned value does not touch it.
// If an element store stops counting its root as a use, DCE drops the
// declaration and the generated code refers to an undeclared variable.
//@cse
double element_decl(double v) {
  Pair p;
  p.a = v;
  p.b = 2.0 * v;
  return v;
}

// The index of a store is an ordinary expression slot: when it is shared with
// another statement it must be collected and rewritten exactly like a value
// operand, so that one definition serves both uses (and both uses point at it).
//@cse
double element_index(double* buf, int i, double v) {
  buf[i * 2] = v;
  return buf[i * 2];
}
