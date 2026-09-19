#!/usr/bin/env bash
# Engine regression tests. Run via `make test` (binaries must be built).
#
#  1. cost regression: regenerate each //@cse fixture and check the post-pass
#     FLOP count against the expected value
#  2. numerical verifiers: compile and run verify_equilibrium / verify_safety
#  3. csegen smoke test: generate tests/ur/*.h without error
#
# If a FreeLB checkout is available (FREELB=... or ~/FreeLB), also run the
# tools/cse Python verifiers against fresh engine output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CSE="${CSE:-$ROOT/bin/cse}"
CSEGEN="${CSEGEN:-$ROOT/bin/csegen}"
CXX="${CXX:-g++}"
FREELB="${FREELB:-$HOME/FreeLB}"

if [[ ! -x "$CSE" || ! -x "$CSEGEN" ]]; then
  echo "error: build first (make). Missing $CSE or $CSEGEN" >&2
  exit 1
fi

# fixture -> expected post-pass FLOP count
declare -A EXPECTED=(
  [test1]=10
  [test_all]=50
  [equilibrium_ref]=89
  [equilibrium_d3q19]=84
  [safety_cases]=20
  [namespace_case]=4
)

echo "=== cost regression ==="
cost_fail=0
for t in test1 test_all equilibrium_ref equilibrium_d3q19 safety_cases namespace_case; do
  "$CSE" "$ROOT/tests/$t.cpp" >/dev/null
  got="$("$CSE" "$ROOT/tests/$t.cpp" -c 2>&1 | sed -n 's/.*After: *\([0-9]*\) flops.*/\1/p')"
  want="${EXPECTED[$t]}"
  if [[ "$got" == "$want" ]]; then
    echo "ok    $t ($got flops)"
  else
    echo "FAIL  $t: got $got flops, want $want"
    cost_fail=1
  fi
done
(( cost_fail == 0 )) || exit 1

echo "=== numerical verifiers ==="
"$CXX" -std=c++17 -O2 -I"$ROOT/tests" "$ROOT/tests/verify_equilibrium.cpp" -o "$ROOT/tests/.ve.tmp"
"$ROOT/tests/.ve.tmp" | grep -q "max abs error"
echo "ok    verify_equilibrium"
"$CXX" -std=c++17 -O2 -I"$ROOT/tests" "$ROOT/tests/verify_safety.cpp" -o "$ROOT/tests/.vs.tmp"
"$ROOT/tests/.vs.tmp" | grep -q "ALL SAFETY CHECKS PASSED"
echo "ok    verify_safety"
rm -f "$ROOT/tests/.ve.tmp" "$ROOT/tests/.vs.tmp"

echo "=== csegen smoke ==="
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for h in "$ROOT"/tests/ur/*.h; do
  out="$TMP/$(basename "${h%.h}").ur.h"
  "$CSEGEN" "$h" "$out" >/dev/null
  test -s "$out"
  echo "ok    $(basename "$h")"
done

if [[ -d "$FREELB/src/lbm" && -f "$FREELB/tools/cse/verify_moment.py" ]]; then
  echo "=== FreeLB verifiers ($FREELB) ==="
  for v in moment equilibrium force; do
    "$CSEGEN" "$FREELB/src/lbm/$v.h" "$TMP/$v.ur.h" >/dev/null
    if out="$(python3 "$FREELB/tools/cse/verify_$v.py" "$FREELB/src/lbm/$v.ur.h" "$TMP/$v.ur.h" 2>&1)"; then
      echo "ok    $v"
    else
      echo "FAIL  $v"
      echo "$out" | tail -20
      exit 1
    fi
  done
else
  echo "=== FreeLB verifiers skipped (no checkout at $FREELB) ==="
fi

echo "=== all engine tests passed ==="
