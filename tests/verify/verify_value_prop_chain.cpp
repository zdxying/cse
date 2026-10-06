// Differential check that the value-propagation chain fix preserves behavior.
//
// tests/fixtures/value_prop_chain.cpp holds, for each case, a `//@cse`-marked
// function and an unmarked `ref_` twin with the same body. This file includes
// the optimized output and compares the two over many inputs.
//
// The defect it guards against is a chain of trivial locals (`double u = t;`
// with `double t = a;`): the substitution resolved one link per use site, and
// the pass then dropped every declaration it had collected, so the emitted code
// still named the removed `t`. That is a compile error in the plain cases and a
// silent wrong binding in the shadow case -- where the value, not the compiler,
// is the only thing that can tell. Both are caught below.
#include <cmath>
#include <cstdio>

#include "value_prop_chain.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-9;
  if (!ok) failures++;
  std::printf("%-24s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // Includes 7.0 so the shadow case is exercised at the value that equals the
  // global it would wrongly bind to (both then agree by accident) and off it.
  const double xs[] = {-3.25, -0.0, 0.0, 1.0, 2.5, 7.0, 11.5};
  for (double x : xs) {
    check("chain2", sf_chain2(x), ref_chain2(x));
    check("chain3", sf_chain3(x), ref_chain3(x));

    VPCBox q;
    q.v = x + 0.25;
    check("chain_member", sf_chain_member(q), ref_chain_member(q));

    check("chain_expr", sf_chain_expr(x), ref_chain_expr(x));
    check("chain_shadow", sf_chain_shadow(x), ref_chain_shadow(x));
  }

  std::printf(failures == 0 ? "\nALL VALUE-PROP-CHAIN CHECKS PASSED\n"
                            : "\n%d VALUE-PROP-CHAIN CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
