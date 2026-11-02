// Shared primitives for the value-level ("defect pin") verifiers.
//
// Each of those verifiers includes one generated `X.cpp.cse`, compares the
// optimized `sf_` against the unoptimized `ref_` twin (or against a hand-written
// expected value), and prints a banner that run_tests.sh greps for.
//
// This header exists only so the same `failures` counter, `check` helper and
// banner are not copied into ~two dozen files. It changes no assertion: the
// default tolerance (1e-12), the compared values and the banner text are
// exactly what the per-file copies used.
//
//   #include "harness.h"
//   ...
//   check("name", got, want);
//   checkInt("count", got, want);
//   return finish("SAFETY");   // prints "ALL SAFETY CHECKS PASSED"
//
// A verifier with an extra helper (array compare, NaN, signed-zero, call
// counting) keeps it in its own file and just uses the shared `failures`.
#pragma once

#include <cmath>
#include <cstdio>

inline int failures = 0;

inline bool near(double a, double b, double tol = 1e-12) {
  return std::fabs(a - b) < tol;
}

inline void check(const char* name, double got, double want, double tol = 1e-12) {
  const bool ok = near(got, want, tol);
  if (!ok) ++failures;
  std::printf("%-20s got=%.6f want=%.6f  %s\n", name, got, want,
              ok ? "OK" : "FAIL");
}

// Tolerance for the twin-comparison verifiers: they compare an optimized
// function against its unoptimized `ref_` twin over many inputs, and under the
// default profile the two formulations may differ by a few ULP once a pass
// reassociates the arithmetic.
//
// Under the conservative profile (`-s`) no pass may reassociate floating point,
// so an optimized `sf_` must reproduce its `ref_` twin bit for bit -- the 1e-9
// slack there would hide a real (if small) miscompile. run_tests.sh defines
// CSE_CONSERVATIVE for the `-s` compile so the same source tightens its check.
#ifdef CSE_CONSERVATIVE
inline void checkNear(const char* name, double got, double want) {
  check(name, got, want, 1e-12);
}
#else
inline void checkNear(const char* name, double got, double want) {
  check(name, got, want, 1e-9);
}
#endif

inline void checkInt(const char* name, long long got, long long want) {
  const bool ok = (got == want);
  if (!ok) ++failures;
  std::printf("%-20s got=%lld want=%lld  %s\n", name, got, want,
              ok ? "OK" : "FAIL");
}

// Print the standard banner and return the process exit code.
inline int finish(const char* label) {
  if (failures == 0) {
    std::printf("\nALL %s CHECKS PASSED\n", label);
  } else {
    std::printf("\n%d %s CHECK(S) FAILED\n", failures, label);
  }
  return failures == 0 ? 0 : 1;
}
