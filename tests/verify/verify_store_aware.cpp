// Differential check that cross-statement rewrites do not outlive a store.
//
// Every case recomputes the unoptimized semantics by hand. The aliasing case is
// the interesting one: `const_alias` is called twice, once with the same buffer
// for both parameters and once with disjoint buffers, because sharing `p[0]`
// across a store to `q[0]` is only observable in the aliased call.
#include <cmath>
#include <cstdio>

#ifdef CSE_INPUT
#include CSE_INPUT
#else
#include "store_aware.cpp.cse"
#endif

int g_calls = 0;

// Helpers the fixture declares; the fixture source is passed through verbatim,
// so only the definitions are missing here.
double getnorm_pair(Pair p) { return p.a + p.b; }
double getsum_array(double* a) { return a[0] + a[1]; }

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-22s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

static void checkInt(const char* name, int got, int want) {
  bool ok = (got == want);
  if (!ok) failures++;
  std::printf("%-22s got=%d want=%d  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // Hoisting `a * x` would let the second use read the pre-`a = 5` value.
  check("cse_across_store", cse_across_store(2.0, 3.0), 2.0 * 3.0 + 5.0 * 3.0);

  // Nothing is written between the two uses, so sharing is valid here and the
  // value must be unchanged either way.
  check("cse_before_store", cse_before_store(2.0, 3.0),
        2.0 * 3.0 + 2.0 * 3.0 + 5.0);

  // Copying the initializer to the use site would read the overwritten `x`/`a`.
  check("vp_across_store", vp_across_store(1.0, 2.0), 1.0);
  check("vp_param_store", vp_param_store(1.0, 2.0), 1.0);

  // Aliased call: s = 1 + 2 = 3, then buf[0] = 9, then t = 9 + 2 = 11.
  double buf[2] = {1.0, 2.0};
  check("const_alias(aliased)", const_alias(buf, buf), 14.0);
  checkInt("const_alias store", buf[0] == 9.0 ? 1 : 0, 1);

  // Disjoint buffers: the same code must still give the plain answer.
  double p2[2] = {1.0, 2.0};
  double q2[2] = {1.0, 2.0};
  check("const_alias(disjoint)", const_alias(p2, q2), 6.0);
  check("read_only_sum", read_only_sum(p2), 6.0);

  // An impure call between the uses must still run exactly once.
  g_calls = 0;
  double cb = call_between(2.0, 3.0);
  int calls = g_calls;
  check("call_between", cb, 12.0);
  checkInt("call_between calls", calls, 1);

  // ---- Element / member stores ------------------------------------------
  // A store through `p.a` / `a[0]` writes the whole root, so a read taken
  // before it must not be moved or shared past it. `Pair` is passed by value,
  // so the store itself is unobservable from here; the returned value is what
  // separates the two behaviours (3.0 is the value before the store, 9.0 the
  // one after).
  Pair pr{3.0, 4.0};
  check("vp_member", vp_member(pr, 9.0), 3.0);
  check("vp_arrow", vp_arrow(&pr, 9.0), 3.0);

  // Same hazard through a call that reads the whole object: sharing the call
  // node folds two reads into one, taken before the store.
  Pair pr2{3.0, 4.0};
  check("cse_member_call", cse_member_call(pr2), (3.0 + 4.0) + (5.0 + 4.0));

  double arr[2] = {1.0, 2.0};
  check("cse_element_call", cse_element_call(arr), (1.0 + 2.0) + (5.0 + 2.0));
  checkInt("cse_element_call store", arr[0] == 5.0 ? 1 : 0, 1);

  // A store to a different object must still leave a shared read of an
  // untouched object alone: the check has to be precise, not blanket.
  Pair pr3{3.0, 4.0};
  double dst[1] = {0.0};
  check("cse_other_object", cse_other_object(pr3, 2.0, dst), 6.0 + 6.0);

  // ---- What the representation change has to preserve --------------------
  // `p` is only ever stored into. The declaration has to survive DCE, which is
  // also what proves the lvalue counts as a use of the root: drop it and this
  // file does not compile.
  check("element_decl", element_decl(3.0), 3.0);

  // A shared index gets one extracted definition that serves both the store and
  // the load. If only one side is rewritten, the other keeps computing it (and
  // the generated code may reference a variable that was never declared).
  double ebuf[4] = {0.0, 0.0, 0.0, 0.0};
  check("element_index", element_index(ebuf, 1, 5.0), 5.0);
  checkInt("element_index store", ebuf[2] == 5.0 ? 1 : 0, 1);

  std::printf(failures == 0 ? "\nALL STORE-AWARE CHECKS PASSED\n"
                            : "\n%d STORE-AWARE CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
