// Value-level twins for the reference read / rebind defect class.
//
// A reference local is a *second name* for another object. Two rewrites used to
// ignore that and silently change the program:
//
//   sf_ref_loopconst  a body-local's initializer is a constant, and the local is
//                     bound by a reference that the body writes through
//                     (`double& r = v; r += 1.0;`) -- in a *nested* loop, so the
//                     outer unroll runs after the inner one has introduced
//                     `v__u0`/`r__u0`. The unroller's alias info used to be
//                     captured before the inner unroll, so it did not know the
//                     fresh `r__u0 -> v__u0` edge, treated `v__u0` as never
//                     reassigned, and inlined it into the binding --
//                     `double& r__u0__u0 = 0.5;`, ill-formed C++. The referent is
//                     now treated as an lvalue binding target that may not be
//                     value-propagated, and the alias info is rebuilt per loop.
//
//   sf_ref_readalias  an expression reads a variable *through* a reference
//                     (`0.5 + s`, where `s` refers to `b`) and `b` is written in
//                     between. CSE hoisted the expression out of the unrolled
//                     loop because it compared the read name (`s`) against a
//                     write set closed to the referent root (`b`) and saw two
//                     disjoint roots. The read is now resolved through the
//                     reference edge before that comparison.
//
//   sf_ref_vp         the same read-through-a-reference hazard on the
//                     value-propagation path (no loop): `t = s` copies the
//                     referent's value, and inlining `t` as `s` after the write
//                     `r = 2.0` reads the new `b` instead of the old one.
//
// All parameters are scalar so gen_twin_main.py exercises them; the twin
// differential runs `sf_` against the untouched `ref_` on the same inputs, bit
// for bit.

//@cse
double sf_ref_loopconst(double a, double b) {
  double s = 0.0;
  for (int j = 0; j < 2; ++j) {
    for (int k = 0; k < 2; ++k) {
      double v = 0.5;
      double& r = v;
      r += 1.0;
      s += v + a;
    }
  }
  return s + b;
}
double ref_ref_loopconst(double a, double b) {
  double s = 0.0;
  for (int j = 0; j < 2; ++j) {
    for (int k = 0; k < 2; ++k) {
      double v = 0.5;
      double& r = v;
      r += 1.0;
      s += v + a;
    }
  }
  return s + b;
}

//@cse
double sf_ref_readalias(double a, double b, double c) {
  double& r = b;
  double& s = r;
  double acc = 0.0;
  for (int k = 0; k < 3; ++k) {
    acc += (0.5 + s);
    r = b * a;
  }
  return acc + b + c;
}
double ref_ref_readalias(double a, double b, double c) {
  double& r = b;
  double& s = r;
  double acc = 0.0;
  for (int k = 0; k < 3; ++k) {
    acc += (0.5 + s);
    r = b * a;
  }
  return acc + b + c;
}

//@cse
double sf_ref_vp(double a, double b) {
  double& r = b;
  double& s = r;
  double t = s;
  r = 2.0;
  return t + b + a;
}
double ref_ref_vp(double a, double b) {
  double& r = b;
  double& s = r;
  double t = s;
  r = 2.0;
  return t + b + a;
}
