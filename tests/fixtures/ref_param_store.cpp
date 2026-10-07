// Regression fixtures for a DCE miscompile: a store to a reference is deleted.
//
// A store to a *reference* writes the caller's object, so it is observable after
// the call returns. That is true whether the reference arrives as a parameter
// (`double& v`) or as a local declaration (`double& r = v;`). DCE treated every
// parameter and every local declaration as private storage and deleted the store
// whenever the name was not read back inside the function, so a plain setter
// (`void setv(double& v) { v = 1.0; }`) compiled to an empty body.
//
// Each `//@cse`-marked function has an unmarked `ref_` twin with the same body.
// The region extractor optimizes the marked ones and copies the twins through
// unchanged, so tests/verify/verify_ref_param_store.cpp can compare both the
// mutated referent and the return value.
//
// A pointer is *not* affected: reassigning a pointer parameter only changes the
// local copy, so those stores stay removable. `sf_ptr_self` pins that so the fix
// cannot be "stop deleting stores to any parameter".

// 1. plain store to a reference parameter, never read back
//@cse
double sf_ref_param(double& v, double x) { v = x * 2.0; return x + 1.0; }
double ref_ref_param(double& v, double x) { v = x * 2.0; return x + 1.0; }

// 2. store to a reference parameter that *is* read back (worked before; guard)
//@cse
double sf_ref_param_read(double& v, double x) { v = x + 3.0; return v * 2.0; }
double ref_ref_param_read(double& v, double x) { v = x + 3.0; return v * 2.0; }

// 3. store through a reference *local*
//@cse
double sf_ref_local(double& v, double x) { double& r = v; r = x - 1.0; return x; }
double ref_ref_local(double& v, double x) { double& r = v; r = x - 1.0; return x; }

// 4. compound store to a reference parameter (worked before; guard)
//@cse
double sf_ref_compound(double& v, double x) { v += x; return x; }
double ref_ref_compound(double& v, double x) { v += x; return x; }

// 5. integer reference: not a FLOP, so only the value check can see it
//@cse
int sf_ref_int(int& v, int x) { v = x * 3; return x + 1; }
int ref_ref_int(int& v, int x) { v = x * 3; return x + 1; }

// 6. a pointer parameter's own value is a local copy: reassigning it is still
//    dead. The caller's pointee must not change either way.
//@cse
double sf_ptr_self(double* p, double x) { p = 0; return x + 1.0; }
double ref_ptr_self(double* p, double x) { p = 0; return x + 1.0; }
