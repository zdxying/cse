// Differential check that a load through a `const T&` is not shared across a
// write through an aliasing non-const reference.
//
// `ref_alias(x, x)`: a = x.m[0] (old), w.m[0] = 9 changes the *same* object, so
// b = 9 and the sum is old + 9. Caching the read gives 2 * old instead.
#include <cmath>
#include <cstdio>

#include "ref_alias.cpp.cse"

static int failures = 0;

int main() {
  RefAliasV x;
  x.m[0] = 1.0;
  x.m[1] = 0.0;
  x.m[2] = 0.0;
  x.m[3] = 0.0;
  const double got = ref_alias(x, x);
  const bool ok = std::fabs(got - 10.0) < 1e-12;
  if (!ok) failures++;
  std::printf("ref_alias(aliased) got=%.3f want=10.000  %s\n", got, ok ? "OK" : "FAIL");

  std::printf(failures == 0 ? "\nALL REF-ALIAS CHECKS PASSED\n"
                            : "\n%d REF-ALIAS CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
