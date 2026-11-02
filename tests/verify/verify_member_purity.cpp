// A user method named after a builtin math function must not be treated as
// pure. `obj.sin(x)` writes `obj`, so two calls have two effects and CSE may
// not merge them. Before the fix, `isPureCallee` matched the bare method name
// against the builtin whitelist and the two calls collapsed into one.
#include <cstdio>

#include "harness.h"
#include "member_purity.cpp.cse"

int main() {
  MemberPurityS o;
  o.s = 0.0;
  double r = member_sin_twice(o, 2.0);
  // Two calls, each `s += 2.0`.
  check("member_sin_twice", r, 4.0);
  return finish("MEMBER-PURITY");
}
