// A numeric kind must survive LoopUnroll's local renaming.
//
// `numKind` is derived metadata (dag_node.h): the builder sets it when it sees a
// declaration. LoopUnroll renames a mutable body-local per iteration
// (`v` -> `v__u0`) via renameLocal, which used to rename the declaration and
// nothing else. The fresh name therefore stayed `Unknown`, and every rewrite
// that needs the kind refused to fire *for the unrolled copies only*:
//
//   `s = v / v;`   folds to `s = 1.0;`   when v's kind is Float,
//                  stays     `s = v/v;` when it is Unknown.
//
// So identical source optimized differently depending on whether its loop was
// unrolled -- a silent capability gap that no value check can see (both forms
// compute the same number, `1.0` vs `v/v` -- well, they differ at v == 0, but
// v is `s`, a running sum, so the fixture must not divide by zero).
//
// The shape check in run_tests.sh asserts the division is gone. The value
// checks here guard the other direction: the fold must not have been applied
// where it changes the answer.
//
//@cse
double sf_kind_through_unroll(int n) {
  double s = 1.0;
  for (int i = 0; i < 2; ++i) {
    double v = s;
    s = v / v;
  }
  return s;
}

double ref_kind_through_unroll(int n) {
  double s = 1.0;
  for (int i = 0; i < 2; ++i) {
    double v = s;
    s = v / v;
  }
  return s;
}

// The same shape without a loop: the kind is known either way, so this one pins
// that the fold itself is correct (and would catch a regression that disabled it
// for both).
//
//@cse
double sf_kind_plain(double s) {
  double v = s;
  return v / v;
}

double ref_kind_plain(double s) {
  double v = s;
  return v / v;
}
