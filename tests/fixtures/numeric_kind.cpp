// Regression fixture for the numeric-kind class: a rewrite that drops the only
// floating operand, or replaces a floating expression with an integer
// constant, silently changes the C++ type of the result. The value only shows
// up when an integer operand is involved -- which no earlier fixture had.
//
// Each //@cse function has an unmarked ref_ twin; verify_numeric_kind.cpp
// compares them at values where the old integer rewrite wrapped.

//@cse
double sf_uint_times_2f(unsigned x) { return x * 2.0; }
double ref_uint_times_2f(unsigned x) { return x * 2.0; }

//@cse
double sf_int_times_2f(int x) { return x * 2.0; }
double ref_int_times_2f(int x) { return x * 2.0; }

//@cse
double sf_neg_one(unsigned a, unsigned b) { return (a * (0.0 - 1.0)) * b; }
double ref_neg_one(unsigned a, unsigned b) { return (a * (0.0 - 1.0)) * b; }

//@cse
double sf_unit_factor(int i) { return (1.0 * i) / 2; }
double ref_unit_factor(int i) { return (1.0 * i) / 2; }

//@cse
double sf_unit_divisor(int i) { return (i / 1.0) / 2; }
double ref_unit_divisor(int i) { return (i / 1.0) / 2; }

//@cse
double sf_zero_sub(int i) { return (i - 0.0) / 2; }
double ref_zero_sub(int i) { return (i - 0.0) / 2; }

//@cse
double sf_self_div(double x) { return (x / x) / 2; }
double ref_self_div(double x) { return (x / x) / 2; }

//@cse
double sf_self_div_chain(double x) { double t = x / x; return t / 2; }
double ref_self_div_chain(double x) { double t = x / x; return t / 2; }

// Recombination must not move a floating sum into an unsigned product:
// a*x + a is a double expression, a*(x+1) wraps x+1. Only the -r shape check
// can see it (the default profile never factors).
//@cse
double sf_recomb_unsigned(double a, unsigned x) { return a * x + a; }
double ref_recomb_unsigned(double a, unsigned x) { return a * x + a; }
