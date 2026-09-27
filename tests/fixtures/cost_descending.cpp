// Cost-model fixture: descending counted loops (negative step) must be
// resolved and scaled, not silently zeroed or collapsed to a single iteration.
//
//   - `for (i = 8; i >= 0; --i)`         -> 9 iterations (inclusive)
//   - `for (j = 6; j > 0; j -= 2)`       -> 3 iterations (exclusive, step 2)
//   - `for (k = 0; k < n; ++k)`          -> unknown runtime bound (flagged)

//@cse
double cost_descending(double* out, int n) {
  double acc = 0.0;
  for (int i = 8; i >= 0; --i) {
    acc += out[i] * out[i];
  }
  for (int j = 6; j > 0; j -= 2) {
    acc += out[j] + 1.0;
  }
  for (int k = 0; k < n; ++k) {
    acc += out[k];
  }
  out[0] = acc;
  return acc;
}
