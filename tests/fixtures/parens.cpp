// Operator-precedence fixtures. Every case is a parenthesized expression whose
// meaning changes if the code generator drops the parentheses.
//
// `childNeedsParens` used to test the parent's left-associativity, which holds
// for every binary operator in this IR, so a right child at equal precedence
// never got parentheses: `a / (b * c)` came out as `a / b * c`. Each shape gets
// its own function so the regression is measured per case.
//
// The `a - (b +/- c)` shapes only diverge under `-s`: with the default profile
// ReassociatePass happens to normalize them into an equivalent form, which is
// why the defect stayed hidden. Both configurations run this fixture.

//@cse
double sub_sub(double a, double b, double c) { return a - (b - c); }
//@cse
//@cse
double sub_add(double a, double b, double c) { return a - (b + c); }
//@cse
//@cse
double add_sub(double a, double b, double c) { return a + (b - c); }
//@cse
//@cse
double div_mul(double a, double b, double c) { return a / (b * c); }
//@cse
//@cse
double div_div(double a, double b, double c) { return a / (b / c); }
//@cse
//@cse
int mul_div(int a, int b, int c) { return a * (b / c); }
//@cse
//@cse
int mod_mod(int a, int b, int c) { return a % (b % c); }
//@cse
//@cse
double nested_right(double a, double b, double c, double d) {
  return a - (b - (c - d));
}
//@cse
//@cse
double left_chain(double a, double b, double c) { return a - b - c; }
//@cse
//@cse
double grouped_left(double a, double b, double c) { return (a - b) - c; }
//@cse
