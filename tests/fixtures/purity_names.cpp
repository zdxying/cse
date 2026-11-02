// The builtin purity whitelist may only decide *qualified* calls.
//
// `isPureCallee` used to match a bare name, so a user-defined free function
// called `max`/`min`/`abs` -- the names users define most often -- was treated
// as `std::max`. CSE then merged two calls with side effects into one. The
// effect oracle cannot see it: the merged node is marked `pure`, and
// `effectSignature` only counts impure calls. The call counter is the oracle.
//
// The value is *not* enough: `max(x, 0.5)` twice and once give the same result
// for a pure-looking body. Only the side effect differs.

#include <cmath>

// Supplied by the verifier: counts calls, so a merged pair is visible.
int purity_calls = 0;

// The user's own `max`. It is not std::max and it has a side effect.
double max(double a, double b) {
    purity_calls += 1;
    return a > b ? a : b;
}

// Two calls to the user's `max` must stay two calls.
//@cse
double user_max_twice(double x) {
    double t = max(x, 0.5);
    double u = max(x, 0.5);
    return t + u;
}

// Control: `std::sqrt` is genuinely pure, so two occurrences may share one
// node. run_tests.sh asserts the generated code still contains exactly one
// `std::sqrt(` call -- i.e. the fix did not disable dedup of the real builtins.
//@cse
double std_sqrt_twice(double x) {
    double t = std::sqrt(x);
    double u = std::sqrt(x);
    return t + u;
}
