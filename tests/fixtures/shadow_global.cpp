// A declaration inside the region may not be confused with a name the region
// *reads* but does not declare.
//
// Variables are interned by name, so `double s = t;` (where `t` is a global)
// and a later `double t = a;` used to be the *same* DAG node. Value propagation
// then treated the global read as an alias of the local and inlined `a` into
// it: `f(2.0)` returned 4 instead of 7.
//
// The builder now remembers every name that resolves outside the region --
// through reads (varRef) and through assignment targets alike -- and
// alpha-renames a later declaration of the same spelling. The first pair puts
// the read *before* the declaration, which is the shape that used to break; the
// `ref_` twin is the same body, so the differential sees any difference.

double shadow_global_t = 5.0;

// The read happens first, then the local shadows the global. The result must be
// the global's value plus the local's.
//@cse
double sf_shadow_before(double a) {
    double s = shadow_global_t;
    double shadow_global_t = a;
    return s + shadow_global_t;
}
double ref_shadow_before(double a) {
    double s = shadow_global_t;
    double shadow_global_t = a;
    return s + shadow_global_t;
}

// A global read inside a nested block, with the shadowing declaration after it
// in the same block. Same hazard, one scope deeper.
//@cse
double sf_shadow_nested(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        s = shadow_global_t;
        double shadow_global_t = a;
        s = s + shadow_global_t;
    }
    return s;
}
double ref_shadow_nested(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        s = shadow_global_t;
        double shadow_global_t = a;
        s = s + shadow_global_t;
    }
    return s;
}

// Control: a declaration that shadows nothing (the name is never read before
// it) must not be renamed -- and in particular must still be inlinable, so this
// case pins that the fix does not disable value propagation wholesale.
//@cse
double sf_no_shadow(double a) {
    double shadow_global_t = a * 2.0;
    double u = shadow_global_t;
    return u + 1.0;
}
double ref_no_shadow(double a) {
    double shadow_global_t = a * 2.0;
    double u = shadow_global_t;
    return u + 1.0;
}

// The *write* shape: the region assigns to the external name before the local
// declaration. An assignment target goes through resolve(), not varRef(), so
// the write alone used to leave the name unregistered and the later
// declaration interned onto the same node -- the store then landed on the
// local (or was lost, depending on order). The observable must therefore see
// the global's value from outside the region too.
double shadow_sink = 0.0;

//@cse
double sf_shadow_write(double a) {
    shadow_global_t = a;
    double shadow_global_t = 1.0;
    shadow_sink = shadow_global_t;
    return shadow_global_t;
}
double ref_shadow_write(double a) {
    shadow_global_t = a;
    double shadow_global_t = 1.0;
    shadow_sink = shadow_global_t;
    return shadow_global_t;
}

// Same shape one scope deeper: the write happens inside an if, the shadowing
// declaration after it in the same block.
//@cse
double sf_shadow_write_nested(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        shadow_global_t = a;
        double shadow_global_t = 2.0;
        s = shadow_global_t;
    }
    return s;
}
double ref_shadow_write_nested(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        shadow_global_t = a;
        double shadow_global_t = 2.0;
        s = shadow_global_t;
    }
    return s;
}
