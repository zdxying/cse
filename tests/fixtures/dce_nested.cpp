// Dead code elimination must reach into nested scopes.
//
// DCE only ever pruned the top-level block: `pruneBlock` walked `block->stmts`
// and never recursed, so a dead declaration or a dead store inside an `if` or a
// `for` body survived every round. CleanupPass already recursed (`pruneNested`);
// the two passes therefore disagreed about what "dead code" means.
//
// The shapes below are the ones that used to survive. A zero-initialized local
// that is written but never read is the case the two passes must agree on:
// dropping the declaration while keeping the store, or vice versa, produces
// code that does not compile.

// A dead zero-init local inside an `if` body. `dead` is written once and never
// read, so both its declaration and the store are removable.
//@cse
double sf_dead_in_if(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        double dead = 0.0;
        dead = 3.0 * a;
        s = a;
    }
    return s;
}
double ref_dead_in_if(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        double dead = 0.0;
        dead = 3.0 * a;
        s = a;
    }
    return s;
}

// The same shape inside a loop body. Unrolling replicates the declaration per
// iteration, so a miss here leaves one dead local per iteration.
//@cse
double sf_dead_in_loop(double a, int k) {
    double s = 0.0;
    for (int i = 0; i < 2; ++i) {
        double dead = 0.0;
        dead = 3.0 * a;
        s = s + a;
    }
    return s;
}
double ref_dead_in_loop(double a, int k) {
    double s = 0.0;
    for (int i = 0; i < 2; ++i) {
        double dead = 0.0;
        dead = 3.0 * a;
        s = s + a;
    }
    return s;
}

// Control: a nested local that *is* read must survive. Recursing must not turn
// DCE into "delete anything inside a conditional".
//@cse
double sf_live_in_if(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        double used = 3.0 * a;
        s = used + 1.0;
    }
    return s;
}
double ref_live_in_if(double a, int k) {
    double s = 0.0;
    if (k > 0) {
        double used = 3.0 * a;
        s = used + 1.0;
    }
    return s;
}
