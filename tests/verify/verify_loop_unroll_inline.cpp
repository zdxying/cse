// Differential check that the loop-unroller no longer inlines a body-local
// initializer past a write to a variable it reads.
//
// tests/fixtures/loop_unroll_inline.cpp holds, for each case, a `//@cse`-marked
// function and an unmarked `ref_` twin with the same body. This file includes
// the optimized output and compares the two over many inputs. The FLOP count is
// identical either way, so only the values can see the miscompile.
#include <cmath>
#include <cstdio>

#include "loop_unroll_inline.cpp.cse"
#include "harness.h"

int main() {
  const double as[] = {-2.5, -1.0, 0.0, 1.0, 3.75, 10.0};
  const double bs[] = {-3.0, 0.5, 2.0, 7.5};
  for (double a : as) {
    checkNear("unroll_write_after", sf_unroll_write_after(a),
          ref_unroll_write_after(a));
    checkNear("unroll_write_block", sf_unroll_write_block(a),
          ref_unroll_write_block(a));
    checkNear("unroll_loopvar", sf_unroll_loopvar(a), ref_unroll_loopvar(a));
    for (int c = 0; c <= 1; ++c) {
      checkNear("unroll_write_cond", sf_unroll_write_cond(a, c),
            ref_unroll_write_cond(a, c));
    }
    for (double b : bs) {
      checkNear("unroll_stable", sf_unroll_stable(a, b), ref_unroll_stable(a, b));
    }
  }

  return finish("LOOP-UNROLL-INLINE");
}
