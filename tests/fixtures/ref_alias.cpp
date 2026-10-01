// A `const T&` parameter is as aliasable as a `const T*` one.
//
// `const` promises only that the referent is not written *through this
// reference*; it says nothing about another reference to the same object. With
//
//     f(const V& v, V& w)   called as   f(x, x)
//
// the write through `w` changes what the second read of `v` sees, so sharing
// that read across the write is wrong. The reads used to be extracted into one
// temporary, which emitted `t + t` where the source computes `a + b` (2.0 vs
// 10.0 for the verifier's inputs).
//
// tests/verify/verify_ref_alias.cpp calls it with both arguments the same
// object, which is the case that must give `a + b` and not `2 * a`.

struct RefAliasV { double m[4]; };

//@cse
double ref_alias(const RefAliasV& v, RefAliasV& w) {
  double a = v.m[0];
  w.m[0] = 9.0;
  double b = v.m[0];
  return a + b;
}
//@cse
