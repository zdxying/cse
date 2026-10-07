// Regression fixtures for a dropped-output miscompile: a `//@cse` region that
// marks a pure data struct (no methods) was emitted as nothing.
//
// `optimizeFunctionsAndStructs` writes the pure data structs out on the first
// *function* iteration, so a region whose only content is such a struct -- and
// with no function anywhere -- produced no text at all. The emitted file then
// referenced a type that no longer existed. The same held for a namespace whose
// only member is a data struct.
//
// The verifier includes the optimized text and *uses* each struct, so a dropped
// definition is a compile error (there is no value to compare against).

// 1. a top-level data struct in its own region
//@cse
struct SRPoint {
  double x;
  double y;
};
//@cse
double sr_sum(SRPoint p) { return p.x + p.y; }

// 2. a namespace whose only member is a data struct
//@cse
namespace sr_ns {
struct SRPair {
  double a;
  double b;
};
}
//@cse
double sr_prod(sr_ns::SRPair p) { return p.a * p.b; }
