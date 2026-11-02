// Twin differential fixture mirroring the scalar *CSE* shapes of the flagship
// fixtures.
//
// `basic_cse.cpp`, `features.cpp` and `namespace_case.cpp` are the canonical
// CSE shapes, but each is pinned only by a FLOP count and a structural grep --
// neither observes the *value* the optimized code computes. A wrong CSE (sharing
// `a*b` where it must not, or dropping a use) leaves the FLOP count and the
// emitted shape looking plausible while changing the result.
//
// Each pair below reproduces one flagship shape with scalar parameters, so the
// generated twin driver (tests/verify/gen_twin_main.py) can call the optimized
// `sf_` and the untouched `ref_` on the same inputs and compare:
//
//   sf_prodchain   <- basic_cse::compute        (chain of products, CSE a*b)
//   sf_featcompute <- features::compute         (a*x shared by t1 and t2)
//   sf_featcond    <- features::conditional     (branch on a*x, sign of val*val)
//   sf_ns          <- namespace_case::cse_ns    (a*b shared three ways)
//   sf_tri         <- cost_nested               (triangular alpha<=beta<3 nest)
//
// The triangular nest is reformulated over scalars: `cost_nested` takes a
// `double*` / `Vector` (outside the twin generator's scalar v1 boundary) and the
// frontend rejects the local array a scalar reformulation would otherwise need,
// so the loop counters feed the arithmetic directly. The flagships are also
// non-compilable as C++ (`features` dereferences `double*` with `->`,
// `cost_nested`'s `Vector` has no operators), which is why they carry no value
// oracle today -- these scalar twins are that oracle.
//
// `cost_nested`'s nest *is* fully unrolled, so its twin is discriminating.
// `cost_descending` is compilable, so it gets a direct absolute-value oracle in
// tests/verify/verify_cost_descending.cpp instead of a (vacuous) twin: the CSE
// leaves its loops alone, only the cost model resolves them.

//@cse
double sf_prodchain(double a, double b, double c, double d, double e) {
    double t1 = a * b * c * d * e;
    double t2 = b * c * d;
    double t3 = c * d;
    double t4 = a * b;
    return t1 + t2 + t3 + t4;
}

double ref_prodchain(double a, double b, double c, double d, double e) {
    double t1 = a * b * c * d * e;
    double t2 = b * c * d;
    double t3 = c * d;
    double t4 = a * b;
    return t1 + t2 + t3 + t4;
}

//@cse
double sf_featcompute(double a, double x, double y, double result, int n) {
    double t1 = a * x + a * y;
    double t2 = a * x * a * x;
    result = t1 + t2;
    return result;
}

double ref_featcompute(double a, double x, double y, double result, int n) {
    double t1 = a * x + a * y;
    double t2 = a * x * a * x;
    result = t1 + t2;
    return result;
}

//@cse
double sf_featcond(double a, double x, int n) {
    double result = 0;
    double val = a * x;
    if (val > 0) {
        result = result + val * val;
    } else {
        result = result - val * val;
    }
    return result;
}

double ref_featcond(double a, double x, int n) {
    double result = 0;
    double val = a * x;
    if (val > 0) {
        result = result + val * val;
    } else {
        result = result - val * val;
    }
    return result;
}

//@cse
int sf_ns(int a, int b, int c) {
    int x = a * b;
    int y = a * b + c;
    return x + y + a * b;
}

int ref_ns(int a, int b, int c) {
    int x = a * b;
    int y = a * b + c;
    return x + y + a * b;
}

//@cse
double sf_tri(double a, double b, int n) {
    double acc = a * b;
    for (unsigned int alpha = 0; alpha < 3; ++alpha) {
        for (unsigned int beta = alpha; beta < 3; ++beta) {
            for (unsigned int k = 0; k < 19; ++k) {
                acc += a * b * (double)(alpha + beta + k);
            }
        }
    }
    for (int i = 0; i < n; ++i) { acc += a * b; }
    return acc;
}

double ref_tri(double a, double b, int n) {
    double acc = a * b;
    for (unsigned int alpha = 0; alpha < 3; ++alpha) {
        for (unsigned int beta = alpha; beta < 3; ++beta) {
            for (unsigned int k = 0; k < 19; ++k) {
                acc += a * b * (double)(alpha + beta + k);
            }
        }
    }
    for (int i = 0; i < n; ++i) { acc += a * b; }
    return acc;
}
