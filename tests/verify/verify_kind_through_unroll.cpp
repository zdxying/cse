// Pins that a numeric kind survives LoopUnroll's per-iteration renaming.
//
// The defect class: `numKind` is derived metadata that the builder attaches when
// it sees a declaration. LoopUnroll renames mutable body-locals
// (`v` -> `v__u0`), and the renamed declaration used to carry no kind, so every
// rewrite that needs one refused to fire for the unrolled copies only. `v / v`
// folded to `1.0` outside a loop and stayed `v / v` inside one.
//
// Value checks alone cannot see this: both forms compute the same number for a
// non-zero `v`. The discriminating assertion is the *shape* (run_tests.sh
// asserts the division is gone from the unrolled body). What the value checks
// here add is the opposite direction -- that the fold was not applied where it
// would be wrong. `x / x` is 1 only for finite non-zero x, so both fixtures keep
// the divisor away from 0 and the harness compares sf_ against the untouched ref_.
#include "harness.h"

#include "kind_through_unroll.cpp.cse"

int main() {
  // The loop body divides a running sum by itself; the sum stays 1.0 after the
  // first iteration, so the divisor is never 0.
  check("unroll_0", sf_kind_through_unroll(2), ref_kind_through_unroll(2));
  check("unroll_1", sf_kind_through_unroll(0), ref_kind_through_unroll(0));
  // The non-loop twin: kind known either way.
  check("plain_0", sf_kind_plain(2.0), ref_kind_plain(2.0));
  check("plain_1", sf_kind_plain(-3.5), ref_kind_plain(-3.5));
  return finish("KIND-THROUGH-UNROLL");
}
