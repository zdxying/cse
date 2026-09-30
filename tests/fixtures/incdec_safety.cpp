// A decrement / increment is not a unary minus / plus.
//
// `++x` and `--x` are encoded as a UnaryOp whose `op` is '+'/'-' and whose
// `name` is the spelling "++"/"--" (see isIncDec). A pass that only inspects
// `op == '-'` therefore mistakes `--x` for `-x`. The algebraic and reassociation
// passes did exactly that: `negInner` peeled a decrement like a negation,
// `a+(-b)→a-b` / `a-(-b)→a+b` dropped the decrement, and additive reassociation
// flattened `--x` into `-x`. Each function below puts a single decrement where
// one of those passes looks. (A double decrement like `(--x) * (--x)` is not
// used: it is undefined behaviour in C++ itself, so it cannot be a differential
// oracle.) tests/verify/verify_incdec_safety.cpp checks the values, which no
// FLOP count can see.

//@cse
double predec_mul(double x, double y) { return (--x) * y; }
//@cse
double predec_add(double a, double x) { return a + (--x); }
//@cse
double sub_predec(double a, double x) { return a - (--x); }
//@cse
double postdec_mul(double x, double y) { return (x--) * y; }
//@cse
double preinc_mul(double x, double y) { return (++x) * y; }
//@cse
double postinc_mul(double x, double y) { return (x++) * y; }
//@cse
