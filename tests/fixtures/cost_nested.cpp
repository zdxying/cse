// Cost-model fixture: nested `dim` loops (triangular), an unknown-bound loop,
// vector arithmetic and a known helper call.
//
// Exercises the redesign:
//   - triangular `alpha/beta` nest counted exactly (not collapsed to 1x)
//   - `i < n` runtime loop flagged as unknown rather than silently counted once
//   - `Vector<T,3>` arithmetic weighted by lane count
//   - `getnorm2` charged its inner dot-product FLOPs

template <typename T, unsigned int D>
struct Vector {
  T v[D];
  T& operator[](unsigned int i) { return v[i]; }
  const T& operator[](unsigned int i) const { return v[i]; }
};

static double getnorm2(const Vector<double, 3>& u) {
  return u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
}

//@cse
double cost_nested(double* out, const Vector<double, 3>& u, int n) {
  double acc = getnorm2(u);
  Vector<double, 3> scaled = u * 2.0;
  acc += scaled[0] * scaled[1];
  for (unsigned int alpha = 0; alpha < 3; ++alpha) {
    for (unsigned int beta = alpha; beta < 3; ++beta) {
      for (unsigned int k = 0; k < 19; ++k) {
        acc += u[alpha] * u[beta] * (double)k;
      }
    }
  }
  for (int i = 0; i < n; ++i) acc += u[0] * 2.0;
  out[0] = acc;
  return acc;
}
