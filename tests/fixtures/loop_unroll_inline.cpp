// Regression fixtures for a loop-unroller miscompile: a body-local initializer
// was inlined past a write to a variable it reads.
//
// `double t = a + 1.0; a = a + 2.0; s += t;` inside an unrolled loop has a
// *pure* initializer, so the unroller judged it inlinable and substituted
// `a + 1.0` at the use site -- which now sits after `a = a + 2.0`, so the use
// reads the new `a`. Purity is not enough: the initializer's free variables must
// also be unmodified between the declaration and the use. (The sibling fix for
// non-shareable loads -- `double t = a[i]; a[i] = 0; use(t);` -- is in
// tests/fixtures/loop_unroll_semantics.cpp.)
//
// Each `//@cse`-marked function has an unmarked `ref_` twin; the verifier
// compares the two over many inputs. The FLOP count is identical either way, so
// only the values catch it.

// 1. the write is a plain assignment later in the body
//@cse
double sf_unroll_write_after(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + 1.0;
    a = a + 2.0;
    s = s + t;
  }
  return s;
}
double ref_unroll_write_after(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + 1.0;
    a = a + 2.0;
    s = s + t;
  }
  return s;
}

// 2. the write sits inside a nested `if`
//@cse
double sf_unroll_write_cond(double a, int c) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a * 2.0;
    if (c) a = a + 1.0;
    s = s + t;
  }
  return s;
}
double ref_unroll_write_cond(double a, int c) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a * 2.0;
    if (c) a = a + 1.0;
    s = s + t;
  }
  return s;
}

// 3. the write sits inside a nested block
//@cse
double sf_unroll_write_block(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + 0.5;
    { a = a + 4.0; }
    s = s + t;
  }
  return s;
}
double ref_unroll_write_block(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + 0.5;
    { a = a + 4.0; }
    s = s + t;
  }
  return s;
}

// 4. control: the initializer reads a variable that is *not* written, so
//    inlining is still expected -- the value must match the twin regardless.
//@cse
double sf_unroll_stable(double a, double b) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = b * 2.0;
    a = a + 1.0;
    s = s + t;
  }
  return s;
}
double ref_unroll_stable(double a, double b) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = b * 2.0;
    a = a + 1.0;
    s = s + t;
  }
  return s;
}

// 5. control: the initializer reads the loop variable (substituted to a
//    constant) and a constant -- both safe to inline.
//@cse
double sf_unroll_loopvar(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + i * 2.0;
    s = s + t;
  }
  return s;
}
double ref_unroll_loopvar(double a) {
  double s = 0.0;
  for (int i = 0; i < 3; ++i) {
    double t = a + i * 2.0;
    s = s + t;
  }
  return s;
}
