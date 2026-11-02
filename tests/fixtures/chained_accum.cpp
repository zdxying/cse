// CleanupPass coverage.
//
// CleanupPass combines a chain of additive updates on one variable into a single
// initializer -- `double x = 0; x = x + a; x = x + b;` becomes `double x = a + b;`
// -- and drops self-assignments. No other fixture exercised that shape, so the
// pass-contract oracle (Phase D5) reported the pass as firing on nothing at all,
// which is indistinguishable from a pass that has been gated shut.
//
// The rewrite is FLOP-neutral (`x = x + a` and `a + x` both cost one add), so the
// pinned cost cannot see a regression; the function names are what the shape
// check in run_tests.sh greps for.

//@cse
double sf_cleanup_chain(double a, double b, double c) {
  double x = 0;
  x = x + a;
  x = x + b;
  x = x + c;
  return x;
}

// The same chain where the accumulator is a parameter, so the first statement is
// a plain assignment rather than a declaration: CleanupPass has to combine an
// assign chain (`combineChainedAssignAssignments`) instead of a declaration one.
//@cse
double sf_cleanup_assign_chain(double a, double b, double acc) {
  acc = 0;
  acc = acc + a;
  acc = acc + b;
  return acc;
}
