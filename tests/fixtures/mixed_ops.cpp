// Node identity: the fields that have to take part in it.
//
// A DAG node is deduplicated by hash, so two nodes are judged to be one only if
// their identity is the same. The identity used to omit the `name` field (which
// carries the cast type and the `++`/`--` spelling) and the `postfix` flag, and
// nothing in the casts below ever recomputed the hash at all -- every one of
// them was built as hash 0 and then compared as "equal". Concretely:
//
//   `-x` and `--x`          both arrived as UnaryOp(op='-')  -> one temporary
//   `(int)x` and `(float)x` both as Cast                     -> one temporary
//
// and the emitted code then evaluated whichever of the two reached the hash map
// first, twice.
//
// Each function is a case whose optimized form must still agree with the
// source; tests/verify/verify_mixed_ops.cpp checks that numerically.

//@cse
double neg_vs_predec(double x) {
    double a = -x;
    double b = --x;
    return a + b;
}
//@cse
double cast_int_vs_float(double x) {
    double p = (int)x;
    double q = (float)x;
    return p + q;
}
//@cse
double cast_same_twice(double x) {
    double p = (int)x;
    double q = (int)x;
    return p + q;
}
//@cse
double neg_and_inner_neg(double x) {
    double a = -x;
    double b = -(-x);
    return a + b;
}
//@cse
double cast_of_inlined_source(double a) {
    double t = a;
    double u = (int)t;
    return u + t;
}
//@cse
