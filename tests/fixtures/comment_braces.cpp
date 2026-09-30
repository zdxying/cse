// Braces that are not code.
//
// The region extractor decided where a region ended by counting raw `{` and `}`
// characters, so a brace inside a comment (or a string literal, or a character
// literal) moved the count. One `}` in a comment ended the region early, the
// truncated fragment failed to parse, and the region was passed through
// unchanged -- the tool silently did nothing, and only the changed exit status
// hinted at it.
//
// Each region below keeps braces inside comments. Both functions must still be
// optimized (the same subexpression twice is what makes that observable) and
// both must still compute the same values.
//
// tests/verify/verify_comment_braces.cpp checks the values.

//@cse
double braces_in_line_comment(double a, double b) {
    // a stray closing brace used to end this region right here: }
    double x = a * b;
    double y = a * b;
    return x + y;
}
//@cse
double braces_in_block_comment(double a, double b) {
    /* a block comment with both braces { } and a quoted one "}" */
    double x = a + b;
    double y = a + b;
    return x + y;
}
//@cse
