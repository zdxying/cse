// Value oracle for the descending / stepped loop-cost fixture.
//
// `cost_descending.cpp` is pinned only by a FLOP count. Its three loops have
// resolved bounds -- a descending `i >= 0` (9 iterations, inclusive), a stepped
// `j > 0; j -= 2` (j = 6,4,2) and a runtime `k < n` -- and the cost model's
// whole job is to count them right. A miscount is invisible in the emitted shape
// (the loops survive verbatim) but wrong in the estimate the cost model reports.
// This verifier recomputes the value with a plain reference loop and compares, so
// a future pass that starts rewriting (or collapsing) these loops is caught.
//
// The pointer parameter puts this fixture outside the twin generator's scalar
// boundary; its sibling `cost_nested` is covered by the scalar twin in
// tests/verify/twins/flagship.cpp (the fixture itself does not compile: its
// `Vector` has no operators). `out[0]` is written only after the sum, so the
// reference reads the original array.
#include <cstdio>

#include "cost_descending.cpp.cse"
#include "harness.h"

namespace {

double ref_descending(const double* out, int n) {
  double acc = 0.0;
  for (int i = 8; i >= 0; --i) acc += out[i] * out[i];
  for (int j = 6; j > 0; j -= 2) acc += out[j] + 1.0;
  for (int k = 0; k < n; ++k) acc += out[k];
  return acc;
}

}  // namespace

int main() {
  // Distinct values so a dropped iteration or a truncated bound changes the sum.
  double out[9];
  for (int i = 0; i < 9; ++i) out[i] = 0.5 * (i + 1);
  const double want = ref_descending(out, 4);

  double in[9];
  for (int i = 0; i < 9; ++i) in[i] = out[i];
  const double got = cost_descending(in, 4);
  check("cost_descending value", got, want);
  check("cost_descending out[0]", in[0], want);

  return finish("COST-DESCENDING");
}
