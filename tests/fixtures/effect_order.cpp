// Reassociation must not move an effectful term past a read of what it writes.
//
// ReassociatePass flattens an additive chain and re-sorts its terms by use
// frequency (and node id), which changes the *evaluation order* of the terms.
// That is only safe when every term is free of observable effects: C++ leaves
// operand order unspecified for side-effect-free operands, but an impure call
// may read or write state another term reads.
//
//   double state = 1.0;
//   double f() { state = 2.0; return 1.0; }
//   return state + f() + state;
//
// Source order: `state` is read (1.0), f() runs and sets state = 2.0, the second
// `state` is read (2.0) -> 4.0. Sorted order (`state + state + f()`): both reads
// happen first -> 3.0. A silent miscompile: the effect multiset is unchanged, so
// the effect-preservation oracle sees nothing; only the value changes.
//
// Every `sf_` here is an optimized twin of the `ref_` with the same bodies. The
// shape check in run_tests.sh additionally asserts the call was not moved ahead
// of a read, because for the *ordering* half a value check needs a global whose
// initializer the fixture can control -- which is what this file does.

// `state` is a mutable global so that reordering is observable. The optimizer
// sees `f` as impure (no declaration of purity), so it must treat any move of
// the call past a `state` read as unsafe.
double state_probe = 1.0;
double bump_probe() {
  state_probe = 2.0;
  return 1.0;
}

//@cse
double sf_effect_order_0() {
  state_probe = 1.0;
  return state_probe + bump_probe() + state_probe;
}

double ref_effect_order_0() {
  state_probe = 1.0;
  return state_probe + bump_probe() + state_probe;
}

// A shape where the call sits first: any reordering would move pure reads ahead
// of it.
//@cse
double sf_effect_order_1() {
  state_probe = 1.0;
  return bump_probe() + state_probe + state_probe;
}

double ref_effect_order_1() {
  state_probe = 1.0;
  return bump_probe() + state_probe + state_probe;
}

// Two effects among pure terms: their *relative* order was already preserved
// (stable_sort), but both could be moved past the pure reads. Pinned here so a
// future "sort harder" fix cannot regress it.
//@cse
double sf_effect_order_2(double a) {
  state_probe = 1.0;
  return a + state_probe + bump_probe() + state_probe;
}

double ref_effect_order_2(double a) {
  state_probe = 1.0;
  return a + state_probe + bump_probe() + state_probe;
}

// Control: a chain with no effects must still be reorderable (that is the
// pass's whole job). run_tests.sh asserts the shape stays reordered.
//@cse
double sf_pure_chain(double a, double b, double c) {
  return a + b + c + a;
}

double ref_pure_chain(double a, double b, double c) {
  return a + b + c + a;
}
