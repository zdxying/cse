// A user-defined free function that happens to share a builtin's name
// (`max`, `min`, `abs`, ...) must not be treated as that builtin. CSE would
// otherwise merge two calls with side effects into one, and no value oracle
// could see it -- the merged node is marked pure, so the effect signature stays
// silent too. The call counter is the oracle.
#include <cstdio>

#include "harness.h"
#include "purity_names.cpp.cse"

int main() {
  // Two calls, two increments.
  purity_calls = 0;
  double r = user_max_twice(2.0);
  check("user_max_twice value", r, 4.0);
  checkInt("user_max_twice calls", purity_calls, 2);

  // Control: std::sqrt really is pure, so the value is unchanged and the
  // generated code still contains a single call (asserted as a shape check in
  // run_tests.sh, which can see the text).
  check("std_sqrt_twice", std_sqrt_twice(9.0), 6.0);

  return finish("PURITY-NAMES");
}
