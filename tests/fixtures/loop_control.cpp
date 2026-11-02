// Loop-control fixtures: `break;` / `continue;` must survive optimization.
//
// This IR has no keyword for them: `break;` arrives as an expression statement
// whose expression is a bare variable named "break" (see isLoopControl in
// ir/stmt_walk.h). CleanupPass removes "useless" bare-variable statements, so it
// used to erase these -- and once CleanupPass learned to recurse into nested
// blocks, it erased them inside a braced `if`/loop body too, silently changing
// control flow.
//
// The `semantics_fixes` twins (`sf_break` / `sf_continue`) use a *brace-less*
// `if (...) break;`, whose branch is a bare statement rather than a Block, so
// the erasure never reached it and the regression went unnoticed. These twins
// put the statement inside a braced block on purpose, and pair it with a value
// oracle (the twin differential) plus a shape check on the emitted keyword.

//@cse
double sf_break_braced(double a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i >= 3) { break; }
    s += a;
  }
  return s;
}
double ref_break_braced(double a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i >= 3) { break; }
    s += a;
  }
  return s;
}

//@cse
double sf_continue_braced(double a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i >= 3) { continue; }
    s += a;
  }
  return s;
}
double ref_continue_braced(double a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i >= 3) { continue; }
    s += a;
  }
  return s;
}

// A `break` in a nested `if` inside a nested block: the erasure descends
// through both scopes.
//@cse
double sf_break_nested(double a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i > 1) {
      if (i >= 5) { break; }
      s += a;
    }
  }
  return s;
}
double ref_break_nested(double a, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    if (i > 1) {
      if (i >= 5) { break; }
      s += a;
    }
  }
  return s;
}
