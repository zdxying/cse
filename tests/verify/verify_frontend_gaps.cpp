// Pins the four frontend shapes that used to make a whole region unparseable:
// `size_t` as a for-init type, top-level `static inline`, `<<`/`>>`, and a
// dereference read/store. See frontend_gaps.cpp for the per-shape reasoning --
// the value checks here are the safety half (the optimizer must not change any
// of these), while run_tests.sh asserts the *optimization* half (these regions
// are actually optimized now, not merely passed through).
#include "harness.h"

#include "frontend_gaps.cpp.cse"

int main() {
  // The sizet-loop fixture indexes a[0..3]; pass a real array (nullptr would
  // dereference, and the point of the fixture is the for-init *type*, not the
  // pointer).
  double arr[4] = {1.0, 2.0, 3.0, 4.0};
  double arr2[4] = {1.0, 2.0, 3.0, 4.0};
  check("sizet_loop_0", sf_sizet_loop(arr), ref_sizet_loop(arr2));
  arr[1] = 10.0; arr2[1] = 10.0;
  check("sizet_loop_1", sf_sizet_loop(arr), ref_sizet_loop(arr2));

  check("static_inline_0", sf_static_inline(2.0), ref_static_inline(2.0));
  check("static_inline_1", sf_static_inline(-0.5), ref_static_inline(-0.5));
  check("plain_helper_0", sf_plain_helper(3.0), ref_plain_helper(3.0));
  check("shift_0", sf_shift(0), ref_shift(0));
  check("shift_1", sf_shift(3), ref_shift(3));
  check("shift_2", sf_shift(31), ref_shift(31));
  check("shift_prio_0", sf_shift_prio(0), ref_shift_prio(0));
  check("shift_prio_1", sf_shift_prio(4), ref_shift_prio(4));
  check("shift_cmp_0", sf_shift_cmp(0, 1), ref_shift_cmp(0, 1));
  check("shift_cmp_1", sf_shift_cmp(10, 10), ref_shift_cmp(10, 10));

  // The deref pair: run against non-aliased pointers only. The default profile
  // licenses noAlias, so passing the same pointer twice would make the
  // *licensed* assumption false -- a property of the caller, not of the
  // optimizer. (The -s profile keeps both loads and is alias-safe; this fixture
  // exercises the shape, not the licence.)
  double x = 1.0, y = 2.0, u = 1.0, v = 2.0;
  check("deref_0", sf_deref(&x, &y), ref_deref(&u, &v));
  x = 5.0; y = -1.0; u = 5.0; v = -1.0;
  check("deref_1", sf_deref(&x, &y), ref_deref(&u, &v));
  return finish("FRONTEND-GAPS");
}
