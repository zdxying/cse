// Regression fixture for CleanupPass's chained-add merge. The merge rewrites
// two sequenced statements into one expression, so it may only fire when the
// accumulator is a private local and every addend is referentially
// transparent. It used to check only that the addend did not *name* the
// accumulator, which let it fuse impure addends and aliasing reference
// parameters.

// Two increments of the same variable: the merged form increments y twice
// without sequencing (undefined behaviour), while the source is well-defined.
//@cse
double sf_inc_chain(double y) {
  double x = 0;
  x = x + (++y);
  x = x + (++y);
  return x * 100.0 + y;
}
double ref_inc_chain(double y) {
  double x = 0;
  x = x + (++y);
  x = x + (++y);
  return x * 100.0 + y;
}

// The accumulator and the addend are the same object when the caller passes
// the same variable twice: the source reads the zeroed value (2), the merged
// assignment reads the caller's old value instead.
//@cse
double sf_ref_chain(double& x, double& q) {
  x = 0;
  x = x + q;
  x = x + 2.0;
  return x;
}
double ref_ref_chain(double& x, double& q) {
  x = 0;
  x = x + q;
  x = x + 2.0;
  return x;
}
