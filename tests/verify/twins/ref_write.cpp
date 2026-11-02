// Twin differential fixture for the reference-alias family (Phase C1).
//
// A write through a reference *local* must invalidate its referent, but
// `collectWriteLocations` records only the written root `r`, not the object it
// refers to. Without the alias closure, value propagation treats `x` as
// unmodified and inlines `double y = x;` to a point after the write:
//
//     sf:  r = 2.0; return x + x;   // = 4.0  (WRONG)
//     ref: double y = x; r = 2.0; return y + x;  // = x_orig + 2.0
//
// `AliasInfo` (src/ir/location.h) closes the written set over reference edges,
// so `r`'s referent `x` is seen as written. Each case below is a different
// shape of that edge: a direct binding, a chain (`s -> r -> x`), and a compound
// store (`r += ...`).

//@cse
double sf_refwrite(double x) {
    double y = x;
    double& r = x;
    r = 2.0;
    return y + x;
}

double ref_refwrite(double x) {
    double y = x;
    double& r = x;
    r = 2.0;
    return y + x;
}

//@cse
double sf_refchain(double x) {
    double y = x;
    double& r = x;
    double& s = r;
    s = 2.0;
    return y + x;
}

double ref_refchain(double x) {
    double y = x;
    double& r = x;
    double& s = r;
    s = 2.0;
    return y + x;
}

//@cse
double sf_refcompound(double x) {
    double y = x;
    double& r = x;
    r += 2.0;
    return y + x;
}

double ref_refcompound(double x) {
    double y = x;
    double& r = x;
    r += 2.0;
    return y + x;
}

// A reference declared in a `for`-init. `AliasInfo::scan` only recognised
// reference declarations inside a Block, so `for (double& r = x; ...)` was
// never recorded: a write through `r` did not close to `x`, and `y = x` was
// forwarded past it.
//@cse
double sf_refforinit(double x, int n) {
    double y = x;
    for (double& r = x; n > 0; --n) { r += 1.0; }
    return y + x;
}

double ref_refforinit(double x, int n) {
    double y = x;
    for (double& r = x; n > 0; --n) { r += 1.0; }
    return y + x;
}

// The same edge with a chain through the `for`-init reference.
//@cse
double sf_refforinit_chain(double x, int n) {
    double y = x;
    for (double& r = x; n > 0; --n) {
        double& s = r;
        s += 1.0;
    }
    return y + x;
}

double ref_refforinit_chain(double x, int n) {
    double y = x;
    for (double& r = x; n > 0; --n) {
        double& s = r;
        s += 1.0;
    }
    return y + x;
}
