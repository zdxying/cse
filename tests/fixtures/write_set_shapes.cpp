// Value-level twins for the write-set defect class.
//
// `verify_write_set.cpp` pins the *shape* of the closed write set through the
// library. These twins pin the *value* the optimized code computes for the same
// shapes end to end, through the real pipeline (the twin differential runs the
// marked `sf_` against the untouched `ref_` on the same inputs).
//
// Every shape here is one that the write-set analysis used to miss, so the pass
// upstream (CSE / ValueProp) forwarded a stale value across a write:
//
//   sf_refinit   a reference declared in a `for`-init writes its referent
//                (`for (double& r = x; ...)`) -- commit 62a400a.
//   sf_refchain  a reference chain `s -> r -> i` must close to the base, so a
//                write through `s` invalidates `i`.
//   sf_idxwrite  a value (`a*b`) must not be reused across a write to one of its
//                operands, inside an unrolled loop.
//   sf_loopref   a loop-body-local whose initializer reads a variable that the
//                body writes *through a reference* must not be inlined across
//                that write. This pins LoopUnroll's body-written set being
//                closed over reference edges (it used to be collected without
//                the closure, so `t` was inlined past `r = 2.0`).
//
// All parameters are scalar so gen_twin_main.py exercises them (its v1 boundary
// skips pointer/array/reference signatures); the pointer-shaped store-index case
// is covered by the library oracle instead.

//@cse
double sf_refinit(double x, int n) {
  double a = x;
  for (double& r = x; n > 0; --n) { r += 1.0; }
  return a + x;
}
double ref_refinit(double x, int n) {
  double a = x;
  for (double& r = x; n > 0; --n) { r += 1.0; }
  return a + x;
}

//@cse
double sf_refchain(double x) {
  double i = x;
  double& r = i;
  double& s = r;
  double a = i;
  s = 2.0;
  return a + i;
}
double ref_refchain(double x) {
  double i = x;
  double& r = i;
  double& s = r;
  double a = i;
  s = 2.0;
  return a + i;
}

//@cse
double sf_idxwrite(double a, double b) {
  double s = 0.0;
  for (int k = 0; k < 4; ++k) {
    double v = a * b;
    s += v;
    a = a + 1.0;
  }
  return s;
}
double ref_idxwrite(double a, double b) {
  double s = 0.0;
  for (int k = 0; k < 4; ++k) {
    double v = a * b;
    s += v;
    a = a + 1.0;
  }
  return s;
}

//@cse
double sf_loopref(double x) {
  double s = 0.0;
  for (int k = 0; k < 4; ++k) {
    double t = x + 1.0;
    double& r = x;
    r = 2.0;
    s += t;
  }
  return s;
}
double ref_loopref(double x) {
  double s = 0.0;
  for (int k = 0; k < 4; ++k) {
    double t = x + 1.0;
    double& r = x;
    r = 2.0;
    s += t;
  }
  return s;
}
