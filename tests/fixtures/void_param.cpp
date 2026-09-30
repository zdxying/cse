// `f(void)` -- the C spelling of an empty parameter list.
//
// `void` is a type keyword, so `parseParamList` took it for a parameter's type and
// then demanded a name where it found `)`. The report was `expected Identifier but
// got RParen`, the region counted as unreadable, and the function was passed
// through unchanged: a function written the C way was silently never optimized.
//
// The last two functions pin that accepting `void` did not replace the parameter
// loop -- an ordinary list still has to parse -- and that a `void` return type
// still works.

static double g_calls = 0.0;

//@cse
double no_params(void) {
    double x = 2.0 * 3.0;
    return x + x;
}
//@cse
double void_list_with_params(double a, double b) {
    double s = a + b;
    return s + s;
}
//@cse
void void_return_and_void_list(void) {
    g_calls = g_calls + 1.0;
}
//@cse
