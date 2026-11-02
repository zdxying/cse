// A constant's int/float kind must survive optimization. `2.0` and `2` have the
// same value but are different C++ expressions: `4.0 / 3` is 1.333... while
// `4 / 3` is 1. The IR used to keep only the value (and a spelling that was not
// part of node identity), so:
//
//   * a folded floating constant was emitted as an integer (`(2.0+2.0)/(1.0+2.0)`
//     became `4 / 3` -> 1.0);
//   * the literals `2.0` and `2` collapsed into one node, so `x/2.0 + y/2`
//     emitted `y/2.0` (floating) instead of `y/2` (integer division).
//
// Each `sf_` twin is optimized and compared against its `ref_` twin by the twin
// differential, which catches the wrong value directly.

//@cse
double sf_divfold(double x) { return (2.0 + 2.0) / (1.0 + 2.0) + x; }
double ref_divfold(double x) { return (2.0 + 2.0) / (1.0 + 2.0) + x; }

//@cse
double sf_intvar_double_divisor(int x) { return x / (1.0 + 1.0); }
double ref_intvar_double_divisor(int x) { return x / (1.0 + 1.0); }

// The two literals `2.0` and `2` must not collapse: `y / 2` stays an integer
// division while `x / 2.0` is floating.
//@cse
double sf_mixed_literals(double x, int y) { return x / 2.0 + y / 2; }
double ref_mixed_literals(double x, int y) { return x / 2.0 + y / 2; }

// A pure integer division must stay integer.
//@cse
double sf_int_division(int a) { return a / 2; }
double ref_int_division(int a) { return a / 2; }

// Exact floating division of constants folds to a float, not an int.
//@cse
double sf_exact_div(double x) { return (1.0 + 2.0) / (1.0 + 1.0) + x; }
double ref_exact_div(double x) { return (1.0 + 2.0) / (1.0 + 1.0) + x; }
