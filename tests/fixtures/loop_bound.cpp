// Loop bounds: only an exactly-integral bound is unrollable.
//
// `for (i = 0; i < N; ++i)` with an integral N is unrolled. With `i < 4.5` the
// loop really runs five times (i = 0..4); the unroller used `static_cast<int>`
// on the bound, turning `4.5` into `4` and silently dropping the last
// iteration. A non-integral (or out-of-range) bound must be left alone.
// tests/verify/verify_loop_bound.cpp checks the value.

//@cse
double float_bound() {
  double s = 0;
  for (int i = 0; i < 4.5; ++i) { s += i; }
  return s;
}
//@cse
