// Pins the two hazards the chained-add merge must not fuse: an impure addend
// (two sequenced increments become one unsequenced expression) and an addend
// that aliases the accumulator (a reference parameter).
#include "harness.h"

#include "cleanup_chain_effects.cpp.cse"

int main() {
  check("inc_chain", sf_inc_chain(0.0), ref_inc_chain(0.0));
  check("inc_chain_y", sf_inc_chain(3.0), ref_inc_chain(3.0));

  // The same object bound to both references is the case the merge breaks.
  double a = 10.0;
  double c = 10.0;
  check("ref_chain_alias", sf_ref_chain(a, a), ref_ref_chain(c, c));

  double e = 5.0, f = 7.0;
  double g = 5.0, h = 7.0;
  check("ref_chain_distinct", sf_ref_chain(e, f), ref_ref_chain(g, h));

  return finish("CLEANUP-CHAIN");
}
