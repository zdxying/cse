// Pins that the passes converge instead of stopping at a hand-picked round cap.
//
// Three passes used to iterate a fixed number of times rather than until a
// fixpoint:
//
//   * ValueProp   `for (int iter = 0; iter < 5; ++iter)`   -- one chain link per
//                 round, so a 60-link chain kept four declarations;
//   * DCE         `iter < 10`
//   * Cleanup     `iter < 10`
//   * CSEPass     `iteration < 100` -- 150 independent opportunities produced
//                 only 100 temporaries.
//
// (LoopUnroll had the same shape and was fixed in 0776dad; that one was a
// miscompile because it erased declarations it had not substituted.)
//
// The failure mode is invisible to every value oracle: a chain that stops
// halfway still computes the right number. What changes is the *shape* -- the
// emitted body keeps declarations, and running the tool a second time finishes
// the job. So this verifier checks both halves:
//
//   1. value: the optimized twin matches the untouched one (guards against
//      "converging" by dropping work that mattered);
//   2. shape: the emitted body is fully collapsed -- asserted in run_tests.sh,
//      because it is a property of the generated text, not of the values.
//
// The shape half is what would have caught the original defect. It lives in
// run_tests.sh so it can grep the emitted file for residue declarations; a
// value check cannot see the difference.
#include "harness.h"

#include "long_chain.cpp.cse"

int main() {
  // The chain is the identity function: whatever the shape, the value must not
  // change. A few inputs, including values that would expose a mix-up between
  // links (a wrong link reads a different variable, all of which hold `a`).
  // No `1e308`: the body doubles its input, so that overflows to inf, and
  // `near()` compares with a tolerance rather than bitwise -- an unrelated
  // limitation of the shared harness, not something this fixture is about.
  check("chain_60_1", sf_chain_60(1.0), ref_chain_60(1.0));
  check("chain_60_2", sf_chain_60(-0.0), ref_chain_60(-0.0));
  check("chain_60_3", sf_chain_60(1e300), ref_chain_60(1e300));
  check("chain_60_4", sf_chain_60(-2.5), ref_chain_60(-2.5));
  return finish("LONG-CHAIN");
}
