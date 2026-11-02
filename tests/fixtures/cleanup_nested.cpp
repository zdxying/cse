// Regression fixtures for CleanupPass nested-block recursion.
//
// Each `//@cse`-marked function has an unmarked `ref_` twin with the same body.
// The region extractor optimizes the marked ones and copies the twins through
// unchanged, so tests/verify/verify_cleanup_nested.cpp can compare the two
// numerically.
//
// CleanupPass only pruned the top-level function-body block: a dead declaration
// or a self-assignment inside an `if` / `for` body or a nested `{ }` was never
// removed. The pass now recurses into nested scopes (threading the
// `allowUnsafeFpIdentities` chain-merge gate into every one).
//
// These functions are value-preserving with or without the removal, so the
// numeric twin check below only proves the recursion did not *break* anything.
// The removal itself is pinned by the shape checks in run_tests.sh.

// 1. A dead zero-init declaration and a self-assignment inside an `if` branch.
//@cse
double sf_nested_if(double* a, int c, double x) {
  double r = x;
  if (c) {
    double dead = 0.0;
    a[0] = a[0];
    r = x + 1.0;
  }
  return r;
}
double ref_nested_if(double* a, int c, double x) {
  double r = x;
  if (c) {
    double dead = 0.0;
    a[0] = a[0];
    r = x + 1.0;
  }
  return r;
}

// 2. Dead code inside a `for` body (runtime bound, so it is not unrolled).
//@cse
double sf_nested_for(double* a, int n, double x) {
  double r = 0.0;
  for (int i = 0; i < n; ++i) {
    double dead = 0.0;
    a[i] = a[i];
    r = r + x;
  }
  return r;
}
double ref_nested_for(double* a, int n, double x) {
  double r = 0.0;
  for (int i = 0; i < n; ++i) {
    double dead = 0.0;
    a[i] = a[i];
    r = r + x;
  }
  return r;
}

// 3. A declaration that IS used inside the nested scope must survive the
//    recursion -- the guard against over-deletion. If it were dropped the
//    emitted code would not compile (or would read the wrong value).
//@cse
double sf_nested_used(double x, int c) {
  double r = 0.0;
  if (c) {
    double t = x * 2.0;
    r = t + 1.0;
  }
  return r;
}
double ref_nested_used(double x, int c) {
  double r = 0.0;
  if (c) {
    double t = x * 2.0;
    r = t + 1.0;
  }
  return r;
}
