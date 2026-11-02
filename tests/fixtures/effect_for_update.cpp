// A `for` update that advances something other than the loop's own counter.
//
// The induction step of a counted loop (`++i` for the `i` declared in the init)
// is loop-control: LoopUnroll removes the loop and its counter, so counting it
// as an effect would make every legitimate unroll look like it dropped one. An
// increment of a *different* variable in the update slot is not loop-control --
// such a loop never matches the counted-loop pattern and is never unrolled -- so
// it is an observable effect. `countStmtEffects` (analysis/effects.cpp) used to
// skip any inc/dec in the update slot, which made this increment invisible to
// the effect-preservation oracle; the shape is pinned by the unit checks in
// tests/verify/verify_effect_preservation.cpp and swept here.

//@cse
double sf_effect_for_update(double g, double n) {
  double s = 0.0;
  // The condition is driven by a second variable so the loop terminates: the
  // point is the update slot's `++g`, which advances something that is NOT the
  // loop counter (i is stepped in the body). The first draft of this fixture
  // wrote `for (int i = 0; i < 3; ++g)` with no other step -- the counter never
  // advanced, so the loop ran forever, which is why this fixture had sf_/ref_
  // twins but no runnable value oracle.
  int i = 0;
  for (; i < 3; ++g) {
    s += g;
    ++i;
  }
  return s + g + n;
}
//@cse
double ref_effect_for_update(double g, double n) {
  double s = 0.0;
  int i = 0;
  for (; i < 3; ++g) {
    s += g;
    ++i;
  }
  return s + g + n;
}
