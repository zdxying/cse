// Pins the inlining fixpoint: a chain of more than four inlinable locals must
// still resolve to a body with no erased-name references. The value check also
// catches a partial substitution that happens to compile.
#include "harness.h"

#include "loop_unroll_chain.cpp.cse"

int main() {
  check("chain_0", sf_chain(0.0, 1.0), ref_chain(0.0, 1.0));
  check("chain_1", sf_chain(3.5, 2.0), ref_chain(3.5, 2.0));
  check("chain_2", sf_chain(-1.0, 0.25), ref_chain(-1.0, 0.25));
  return finish("LOOP-UNROLL-CHAIN");
}
