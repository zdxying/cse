// Regression fixtures for a dropped-output miscompile: a `//@cse` marker in
// front of a top-level declaration swallowed that declaration into the next
// function's region, where the parser discarded it.
//
// The region extractor used to keep accumulating until it saw a balanced
// `{ ... }`, so `//@cse` + `double g = 5.0;` + (next function) became one region
// holding the declaration *and* the function. `optimizeRegion` only re-emits the
// functions/structs it parsed, so the declaration vanished from the output and
// the file no longer compiled. A top-level `;` now ends the region, which leaves
// the declaration in its own (pass-through) region.
//
// The verifier includes the optimized text and reads every declaration, so a
// dropped one is a compile error.

// 1. a scalar global in front of a marked function
//@cse
double rt_scale = 3.0;
//@cse
double rt_apply(double x) { return x * rt_scale; }

// 2. a scalar global in front of an *unmarked* function (no second marker)
//@cse
double rt_bias = 1.5;
double rt_shift(double x) { return x + rt_bias; }

// 3. a brace-initialized global array
//@cse
double rt_tab[3] = {1.0, 2.0, 3.0};
//@cse
double rt_tab_sum(void) { return rt_tab[0] + rt_tab[1] + rt_tab[2]; }
