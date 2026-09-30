// Dead stores whose value is not the whole story.
//
// DCE removed any assignment whose target was never read again, checking only
// for impure *calls* in the value. Arithmetic is pure, so that looked safe -- but
// two other things in the same expression are not:
//
//   * `++` / `--` write their operand. `unused = y++;` increments `y`.
//   * a target that is not a local of this function may be a global, and the
//     write outlives the call. (`g_written` below, set to a factor the verifier
//     then reads back.)
//
// The last function is the case that must keep working: a genuinely dead store
// of a pure value still goes.

static double g_calls = 0.0;
static double g_written = 0.0;

static double note_call(double v) {
    g_calls += 1.0;
    return v;
}

//@cse
double inc_in_dead_store(double a) {
    double y = a;
    double unused = 0.0;
    unused = y++;
    return y;
}
//@cse
double dec_in_dead_store(double a) {
    double y = a;
    double unused = 0.0;
    unused = --y;
    return y;
}
//@cse
double write_through_global(double a) {
    g_written = a;
    return a;
}
//@cse
double call_in_dead_store(double a) {
    double unused = 0.0;
    unused = note_call(a);
    return a;
}
//@cse
double dead_pure_store(double a, double b) {
    double unused = 0.0;
    unused = a * b;
    return a + b;
}
//@cse
