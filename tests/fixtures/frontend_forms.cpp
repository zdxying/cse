// The forms a real kernel writes: postfix increment, unary plus, and
// scientific-notation literals.
//
// None of the three used to parse. The postfix forms were the expensive ones:
// `for (int i = 0; i < n; i++)` is the most common loop in C++, and it made the
// driver abort before it wrote any output file at all. It is also why the
// engine's own 23 loops -- every one of them written `++i` -- never noticed.
// `1e16` lexed as the number 1 followed by the identifier `e16`.

//@cse
double counted_postfix(double a, double b) {
    for (int i = 0; i < 4; i++) {
        a += b;
    }
    return a;
}
//@cse
double postfix_loop(double a, double b, int n) {
    for (int i = 0; i < n; i++) {
        a += b;
    }
    return a;
}
//@cse
double postfix_value(double x) {
    double y = x;
    double z = y++;
    return y * 10.0 + z;
}
//@cse
double postfix_stmt(double x) {
    double y = x;
    y++;
    return y;
}
//@cse
double plus_vs_preinc(double x) {
    double a = +x;
    double b = ++x;
    return a + b;
}
//@cse
double sci_notation(double x) {
    double b = 1e16;
    double c = 6.02e23;
    double d = 1e-9;
    return b + c + d + x;
}
//@cse
double sci_upper(double x) {
    double g = 2.5E-3;
    return g * x;
}
//@cse
