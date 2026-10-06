// Regression fixtures for two loop-unroller miscompiles.
//
// Each `//@cse`-marked function has an unmarked `ref_` twin with the same body.
// The region extractor optimizes the marked ones and copies the twins through
// unchanged, so tests/verify/verify_loop_unroll_semantics.cpp can compare the
// two numerically. The cases cover:
//
//   1. a compound store to an element inside an unrolled loop (`out[i] += x`);
//   2. a compound store to a member inside an unrolled loop (`p[i].v += x`);
//   3. a loop-body local whose initializer is a non-shareable load, read before
//      a store to the same location (`double t = a[i]; a[i] = 0; use(t);`);
//   4. the same with a binary initializer (`double t = a[i] * b; ...`).
//
// (1)-(2): the unroller's statement clone dropped `AssignIR::compoundOp`, so
// `out[i] += x` unrolled to `out[i] = x` -- the read of the old value was lost.
//
// (3)-(4): the unroller's inlinability test asked only `!hasImpureCall`, which
// does not see a non-shareable load (an element/member read through a root that
// may be written). The local was deleted and its load moved to each use, past
// the intervening store, so the sum read the zeroed array.
//
// Both are miscompiles that no FLOP count can see: (1)-(2) keep the same number
// of stores, and (3)-(4) keep the same number of loads.

struct LUBox {
  double v;
};

// 1. compound store to an element inside an unrolled loop
//@cse
double sf_cmp_elem_loop(double* out, double x) {
  for (int i = 0; i < 4; ++i) out[i] += x;
  return out[0];
}
double ref_cmp_elem_loop(double* out, double x) {
  for (int i = 0; i < 4; ++i) out[i] += x;
  return out[0];
}

// 2. compound store to a member inside an unrolled loop
//@cse
double sf_cmp_member_loop(LUBox* p, double x) {
  for (int i = 0; i < 4; ++i) p[i].v += x;
  return p[0].v;
}
double ref_cmp_member_loop(LUBox* p, double x) {
  for (int i = 0; i < 4; ++i) p[i].v += x;
  return p[0].v;
}

// 3. a loop-body local read before a store to the same location
//@cse
double sf_load_then_store(double* a) {
  double s = 0;
  for (int i = 0; i < 4; ++i) {
    double t = a[i];
    a[i] = 0;
    s += t;
  }
  return s;
}
double ref_load_then_store(double* a) {
  double s = 0;
  for (int i = 0; i < 4; ++i) {
    double t = a[i];
    a[i] = 0;
    s += t;
  }
  return s;
}

// 4. the same with a binary initializer (the non-shareable load sits under a
//    BinaryOp whose own `pure` flag is true)
//@cse
double sf_load_then_store_bin(double* a, double b) {
  double s = 0;
  for (int i = 0; i < 4; ++i) {
    double t = a[i] * b;
    a[i] = 0;
    s += t;
  }
  return s;
}
double ref_load_then_store_bin(double* a, double b) {
  double s = 0;
  for (int i = 0; i < 4; ++i) {
    double t = a[i] * b;
    a[i] = 0;
    s += t;
  }
  return s;
}
