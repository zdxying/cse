// Bitwise-operator fixtures.
//
// Before B3 the lexer silently dropped a lone `|` and split `%=`, and the
// parser had no precedence levels between `&&` and `==`. A region using `|`,
// `^`, `&` or `~` therefore either failed to parse or was regrouped.
//
// The round-trip oracle walks every case through parse -> build -> codegen ->
// parse and asserts the two IR trees are structurally identical. That is what
// pins the *precedence*: if codegen ranked `|` above `&`, `(a | b) & c` would
// print as `a | b & c` and re-parse as `a | (b & c)` -- a different tree.

//@cse
int bit_or_and(int a, int b, int c) { return (a | b) & c; }
//@cse
int bit_and_or(int a, int b, int c) { return a & (b | c); }
//@cse
int bit_xor_or(int a, int b, int c) { return a ^ b | c; }
//@cse
int bit_or_xor(int a, int b, int c) { return a | (b ^ c); }
//@cse
int bit_not_sum(int a, int b) { return ~a + b; }
//@cse
int bit_not_group(int a, int b) { return ~(a + b); }
//@cse
int bit_vs_eq(int a, int b, int c) { return a & b == c; }
//@cse
int compound_bit(int a, int b) {
    int x = a;
    x |= b;
    x &= b;
    x ^= b;
    x %= b;
    return x;
}
