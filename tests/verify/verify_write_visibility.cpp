// Differential check that a write sitting in a declaration's initializer is
// still seen by the passes that ask "may this have been written in between".
//
// The second case is the one that was already correct: it reads the incremented
// variable, so the wrongly inlined form happens to agree with the source. It is
// here to pin that fixing the scan does not lose that optimization.
#include <cmath>
#include <cstdio>

#include "write_visibility.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-26s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // 1. `y` is incremented inside an initializer, and a *later* statement reads
  //    the original value of `a`. 100a + (a+1).
  check("inc_before(1)", inc_in_decl_before_read(1.0), 100.0 * 1.0 + (1.0 + 1.0));
  check("inc_before(2.5)", inc_in_decl_before_read(2.5), 100.0 * 2.5 + (2.5 + 1.0));
  check("inc_before(-3)", inc_in_decl_before_read(-3.0), 100.0 * -3.0 + (-3.0 + 1.0));

  // 2. The same shape, but reading after the increment: 100(a+1) + (a+1).
  check("inc_after(1)", inc_in_decl_after_read(1.0), 100.0 * 2.0 + 2.0);
  check("inc_after(2.5)", inc_in_decl_after_read(2.5), 100.0 * 3.5 + 3.5);

  // 3. Decrement inside an initializer: 100a + (a-1).
  check("dec_before(1)", dec_in_decl_before_read(1.0), 100.0 * 1.0 + (1.0 - 1.0));
  check("dec_before(2.5)", dec_in_decl_before_read(2.5), 100.0 * 2.5 + (2.5 - 1.0));

  // 4. The write is inside a branch of a conditional, so it is only sometimes
  //    performed -- the read afterwards must not be folded either way.
  check("ternary(1,c=1)", inc_in_ternary_branch(1.0, 1), 100.0 + 2.0);
  check("ternary(1,c=0)", inc_in_ternary_branch(1.0, 0), 100.0 + 1.0);
  check("ternary(2.5,c=1)", inc_in_ternary_branch(2.5, 1), 250.0 + 3.5);

  std::printf(failures == 0 ? "\nALL WRITE-VISIBILITY CHECKS PASSED\n"
                            : "\n%d WRITE-VISIBILITY CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
