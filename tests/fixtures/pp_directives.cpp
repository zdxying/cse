// A preprocessor directive inside a //@cse region.
//
// The frontend does not model directives: there is no AST node for one, so a
// region regenerated from the AST loses the guard and resurrects the code it
// protects -- `#if 0 return -1.0;` becomes the function's result. The driver
// therefore passes such a region through verbatim.
//
// The lexer is the authority for "is this a directive": it emits the line as a
// PreprocessorDirective token, so a `#` inside a string or a block comment is
// ordinary text. That distinction cuts both ways, and both directions were
// wrong when this was decided by scanning raw lines for a leading '#':
//
//   * `/* note */ #if 0` -- the line starts with '/', so the raw scan missed
//     the directive; the region was "optimized" and `return -1.0;` came back to
//     life. (A comment is replaced by a space before directives are recognized,
//     so this IS a directive.)
//   * a block comment whose body has a line starting with `#if 0` -- the raw
//     scan saw a directive that does not exist, and an optimizable region was
//     needlessly passed through.
//
//@cse
double sf_guarded(double x) {
#if 0
  return -1.0;
#endif
  return x + x;
}

double ref_guarded(double x) {
#if 0
  return -1.0;
#endif
  return x + x;
}

// A directive in a region that is otherwise fully optimizable. Passing the
// region through is the conservative answer: the guard is kept, and so is the
// un-optimized `t + t` (the optimizer never sees this body).
//
//@cse
double sf_scale_twice(double a) {
#if 1
  double t = a * 2.0;
  return t + t;
#endif
}

double ref_scale_twice(double a) {
  double t = a * 2.0;
  return t + t;
}

// A comment *before* the `#` on the same line. C++ replaces a comment with one
// space before recognizing directives, so this is a directive and the region
// must be passed through -- otherwise `#if 0`'s body becomes live.
//
//@cse
double sf_comment_then_directive(double x) {
  /* note */ #if 0
  return -1.0;
  /* note */ #endif
  return x + x;
}

double ref_comment_then_directive(double x) {
  /* note */ #if 0
  return -1.0;
  /* note */ #endif
  return x + x;
}

// The mirror image: a *comment* that happens to contain a line starting with
// '#' is not a directive, so this region stays optimizable. The shape check in
// run_tests.sh asserts it was actually optimized -- a value check cannot tell
// "optimized correctly" from "passed through unchanged".
//
//@cse
double sf_comment_only(double x) {
  /* a block comment mentioning
     #if 0
     and #endif must not be read as a directive */
  double a = x * x;
  double b = x * x;
  return a + b;
}

double ref_comment_only(double x) {
  /* a block comment mentioning
     #if 0
     and #endif must not be read as a directive */
  double a = x * x;
  double b = x * x;
  return a + b;
}
