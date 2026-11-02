// Four frontend gaps that used to make an entire region unparseable.
//
// None of these ever produced wrong code -- the region was skipped and passed
// through -- but they silently dropped every optimization in the region, and
// three of the four spellings are ordinary production-code shapes:
//
//   * `size_t` as a for-init type (the ordinary way to index an array);
//   * `static inline double f(...)` at top level (how production headers
//     declare kernels);
//   * `1 << k` / `a >> 2` (bit tricks in kernels);
//   * `*p` reads and `*p = v` stores (pointer-style kernels).
//
// A value check cannot tell "optimized correctly" from "passed through
// unchanged" -- both compute the same number. What pins the fix is the shape:
// run_tests.sh asserts these regions are actually optimized (a `_cse_` temporary
// appears, or the loop is unrolled), which is the property that was lost.
//
//@cse
double sf_sizet_loop(double* a) {
  double s = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    s = s + a[i];
  }
  return s;
}

//@cse
double ref_sizet_loop(double* a) {
  double s = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    s = s + a[i];
  }
  return s;
}

// Two regions: the first is a static inline function (the frontends that skipped
// it reported "unmodeled top-level construct"). The second is a plain function
// so the fixture has at least one region that was always optimized -- run_tests.sh
// greps the whole file for `_cse_` anyway; this one keeps the optimized count
// above zero even if the modifier handling regresses to a skip.
//@cse
static inline double sf_static_inline(double a) {
  double p = a * a;
  double q = a * a;
  return p + q;
}

//@cse
double ref_static_inline(double a) {
  double p = a * a;
  double q = a * a;
  return p + q;
}

//@cse
//@cse
double sf_plain_helper(double a) {
  double p = a * a;
  double q = a * a;
  return p + q;
}

//@cse
double ref_plain_helper(double a) {
  double p = a * a;
  double q = a * a;
  return p + q;
}

// Shifts. The precedence assertions live in run_tests.sh's emitted-text checks;
// here we pin the values.
//@cse
int sf_shift(int k) {
  return 1 << k;
}

//@cse
int ref_shift(int k) {
  return 1 << k;
}

//@cse
//@cse
int sf_shift_prio(int k) {
  // Must be read as 1 << (k + 1): `+` binds tighter than `<<`.
  return 1 << k + 1;
}

//@cse
int ref_shift_prio(int k) {
  return 1 << k + 1;
}

//@cse
//@cse
int sf_shift_cmp(int a, int b) {
  // Must be read as a < (b << 1): `<<` binds tighter than `<`.
  return a < b << 1;
}

//@cse
int ref_shift_cmp(int a, int b) {
  return a < b << 1;
}

// A dereference read and a store through a dereference. Lowered to `p[0]` /
// `q[0]`, so they share the pointer-load machinery (isReadOnlyRoot decides
// shareability exactly as for `p[0]`).
//@cse
//@cse
double sf_deref(double* p, double* q) {
  double a = *p;
  *q = 2.0;
  return a + *q;
}

//@cse
double ref_deref(double* p, double* q) {
  double a = *p;
  *q = 2.0;
  return a + *q;
}
