// The int/float kind of a `* 0` / `x - x` / `0 /` fold.
//
// These identities replace a whole expression with a constant, so the constant
// has to carry the expression's numeric kind. `(0 * x + 2) / 4` is the sharp
// shape: for a double `x` it is 0.5, and emitting an integer `0` turns the
// enclosing division into integer division and yields 0. For an int `x` it is 0,
// and emitting `0.0` turns it into 0.5. The same expression therefore detects a
// wrong kind in either direction.
//
// Both cases are values, not shapes: the twins below compare numbers, so a fold
// that picks the wrong constant kind fails on the value rather than on the
// emitted text.
//
// The awkward part is a zero product whose other operand has no provable kind
// (a load, a call, a template-dependent member): `joinNumericKind` reports
// Unknown and the fold is skipped, which is what used to leave every
// `0.0 * a[i]` -- and every FreeLB lattice `0 * cell[i]` -- in the output. Two
// rules resolve it, and each must keep the kind right:
//
//   * a floating *literal* fixes the product's type by the usual arithmetic
//     conversions, so `0.0 * a[i]` is a double product with no assumption;
//   * `CSEConfig::assumeFloatOperands` additionally says an unknown operand is
//     floating, so the integer literal in `0 * cell[i]` folds too -- to `0.0`.

// A load of unknown kind under a *floating* zero literal: no assumption needed,
// and the fold must produce a floating zero (0.5, not 0).
//@cse
double sf_float_literal(double* c) {
  return (0.0 * c[0] + 2.0) / 4.0;
}
double ref_float_literal(double* c) {
  return (0.0 * c[0] + 2.0) / 4.0;
}

// The same with an *integer* zero literal. This is the case that needs
// `assumeFloatOperands`; folding it to a floating zero is the license, and it
// must still be floating (0.5, not 0).
//@cse
double sf_integer_literal(double* c) {
  return (0 * c[0] + 2) / 4;
}
double ref_integer_literal(double* c) {
  return (0 * c[0] + 2) / 4;
}

// Integer operands only. The product is provably an int, so the fold stays an
// integer zero and the division stays an integer division: 0, never 0.5. This
// is the assertion that `assumeFloatOperands` cannot override a *proven* kind.
//@cse
int sf_int_proven(int k) {
  return (0 * k + 2) / 4;
}
int ref_int_proven(int k) {
  return (0 * k + 2) / 4;
}

// A floating literal on the other side of a load: the same proof, applied
// through `x - x` and `0 / x`.
//@cse
double sf_sub_self(double* c) {
  return (c[0] - c[0] + 2.0) / 4.0;
}
double ref_sub_self(double* c) {
  return (c[0] - c[0] + 2.0) / 4.0;
}
