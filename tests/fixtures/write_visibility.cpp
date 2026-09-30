// Where a write hides.
//
// The write scan enumerated statement kinds and recursed into blocks, loops and
// branches -- but never looked inside a declaration's initializer. `double z =
// ++y;` therefore did not count as writing `y`, `double y = a;` was inlined at
// every use, and a use that came after the increment read the value from before
// it.
//
// Every case below is written so that the source and the wrongly optimized form
// give different numbers; tests/verify/verify_write_visibility.cpp checks them.

//@cse
double inc_in_decl_before_read(double a) {
    double y = a;
    double z = ++y;
    return a * 100.0 + z;
}
//@cse
double inc_in_decl_after_read(double a) {
    double y = a;
    double z = ++y;
    return y * 100.0 + z;
}
//@cse
double dec_in_decl_before_read(double a) {
    double y = a;
    double z = --y;
    return a * 100.0 + z;
}
//@cse
double inc_in_ternary_branch(double a, int c) {
    double y = a;
    double z = c ? ++y : y;
    return a * 100.0 + z;
}
//@cse
