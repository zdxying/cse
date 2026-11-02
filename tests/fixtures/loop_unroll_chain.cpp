// Regression fixture for LoopUnroll's local inlining. It erased every
// inlinable declaration first, then substituted in a loop capped at four
// rounds; a chain longer than the cap kept referencing names whose
// declarations were already gone, and the emitted code did not compile.
// Twenty links make the chain longer than any plausible cap.

//@cse
double sf_chain(double s, double x) {
  for (int i = 0; i < 2; ++i) {
    double a0 = x + 1.0;
    double a1 = a0 + 2.0;
    double a2 = a1 + 2.0;
    double a3 = a2 + 2.0;
    double a4 = a3 + 2.0;
    double a5 = a4 + 2.0;
    double a6 = a5 + 2.0;
    double a7 = a6 + 2.0;
    double a8 = a7 + 2.0;
    double a9 = a8 + 2.0;
    double a10 = a9 + 2.0;
    double a11 = a10 + 2.0;
    double a12 = a11 + 2.0;
    double a13 = a12 + 2.0;
    double a14 = a13 + 2.0;
    double a15 = a14 + 2.0;
    double a16 = a15 + 2.0;
    double a17 = a16 + 2.0;
    double a18 = a17 + 2.0;
    double a19 = a18 + 2.0;
    s = s + a19;
  }
  return s;
}
double ref_chain(double s, double x) {
  for (int i = 0; i < 2; ++i) {
    double a0 = x + 1.0;
    double a1 = a0 + 2.0;
    double a2 = a1 + 2.0;
    double a3 = a2 + 2.0;
    double a4 = a3 + 2.0;
    double a5 = a4 + 2.0;
    double a6 = a5 + 2.0;
    double a7 = a6 + 2.0;
    double a8 = a7 + 2.0;
    double a9 = a8 + 2.0;
    double a10 = a9 + 2.0;
    double a11 = a10 + 2.0;
    double a12 = a11 + 2.0;
    double a13 = a12 + 2.0;
    double a14 = a13 + 2.0;
    double a15 = a14 + 2.0;
    double a16 = a15 + 2.0;
    double a17 = a16 + 2.0;
    double a18 = a17 + 2.0;
    double a19 = a18 + 2.0;
    s = s + a19;
  }
  return s;
}
