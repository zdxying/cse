// Regression fixtures for a CleanupPass miscompile: a declaration read only as a
// store index is deleted.
//
// Each `//@cse`-marked function has an unmarked `ref_` twin with the same body.
// The region extractor optimizes the marked ones and copies the twins through
// unchanged, so tests/verify/verify_cleanup_store_index.cpp can compare the two
// numerically.
//
// `a[x] = v;` owns two expressions -- the lvalue, whose index is an ordinary
// read of `x`, and the value being stored. Cleanup's use counter enumerated
// statement kinds by hand and listed only the *value* of an Assign, so `x`
// looked unused and its zero/const-initialized declaration was removed. The
// emitted code then referenced a name that no longer existed (a compile error),
// or -- when an outer/global of the same name existed -- silently bound that
// other variable instead.
//
// The locals are initialized with `+1`, not `1`: ValueProp treats a bare
// constant as trivial and inlines it, but Cleanup's `isEffectivelyConstant`
// also accepts a unary-plus-of-constant, which ValueProp leaves behind. That is
// what lets the declaration survive all the way to Cleanup.

struct CSIBox {
  double v;
};

// A global the shadow case re-declares locally. If the local is dropped, the
// index binds here (an integer, so it compiles) and the store goes to the wrong
// slot.
int csi_shadow = 0;

// 1. the index appears only in the store lvalue
//@cse
double sf_store_index(double* a) {
  int x = +1;
  a[x] = 5.0;
  return a[1];
}
double ref_store_index(double* a) {
  int x = +1;
  a[x] = 5.0;
  return a[1];
}

// 2. the index is part of an expression inside the lvalue
//@cse
double sf_store_index_expr(double* a) {
  int x = +1;
  a[x + 1] = 7.0;
  return a[2];
}
double ref_store_index_expr(double* a) {
  int x = +1;
  a[x + 1] = 7.0;
  return a[2];
}

// 3. a member element store: the index sits under `p[i].v`
//@cse
double sf_store_member_index(CSIBox* p) {
  int i = +1;
  p[i].v = 3.0;
  return p[1].v;
}
double ref_store_member_index(CSIBox* p) {
  int i = +1;
  p[i].v = 3.0;
  return p[1].v;
}

// 4. silent wrong binding: the index local shadows an int global of the same
//    name, so a dropped declaration compiles and reads the wrong slot.
//@cse
double sf_store_index_shadow(double* a) {
  int csi_shadow = +1;
  a[csi_shadow] = 9.0;
  return a[1];
}
double ref_store_index_shadow(double* a) {
  int csi_shadow = +1;
  a[csi_shadow] = 9.0;
  return a[1];
}
