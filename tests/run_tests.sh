#!/usr/bin/env bash
# Engine regression tests. Run via `make test` (binaries must be built).
#
#  1. cost regression : optimize each //@cse fixture and check the post-pass
#                       FLOP count against the expected value
#  2. numerical       : compile/run the verifiers against freshly generated
#                       output for the equilibrium and safety fixtures
#  3. csegen          : generate tests/csegen/*.h fragments and sanity-check
#                       that representative specializations were emitted
#  4. FreeLB (opt.)   : if a FreeLB checkout is available (FREELB=... or
#                       ~/FreeLB), run the lattice drift guard and the
#                       tools/cse Python verifiers
#
# All tool output (*.cse, *.ur.h, compiled verifiers) is written to a temp dir;
# the source tree is never modified.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURES="$ROOT/tests/fixtures"
VERIFY="$ROOT/tests/verify"
CSEGEN_DIR="$ROOT/tests/csegen"
CSE="${CSE:-$ROOT/bin/cse}"
CSEGEN="${CSEGEN:-$ROOT/bin/csegen}"
CXX="${CXX:-g++}"
FREELB="${FREELB:-$HOME/FreeLB}"

if [[ ! -x "$CSE" || ! -x "$CSEGEN" ]]; then
  echo "error: build first (make). Missing $CSE or $CSEGEN" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# fixture (in tests/fixtures) -> expected post-pass FLOP count
declare -A EXPECTED=(
  [basic_cse]=10
  [features]=50
  [namespace_case]=4
  [equilibrium_d3q19]=84
  [safety_cases]=20
)
# Keep a deterministic order (associative array iteration is unspecified).
FIXTURE_ORDER=(
  basic_cse features namespace_case equilibrium_d3q19 safety_cases
)

echo "=== cost regression ==="
cost_fail=0
for name in "${FIXTURE_ORDER[@]}"; do
  src="$WORK/$name.cpp"
  cp "$FIXTURES/$name.cpp" "$src"
  # A single `-c` run optimizes, writes $src.cse (used by the verifiers below)
  # and reports the cost.
  got="$("$CSE" "$src" -c 2>&1 | sed -n 's/.*After: *\([0-9]*\) flops.*/\1/p')"
  want="${EXPECTED[$name]}"
  if [[ "$got" == "$want" ]]; then
    echo "ok    $name ($got flops)"
  else
    echo "FAIL  $name: got $got flops, want $want"
    cost_fail=1
  fi
done
(( cost_fail == 0 )) || exit 1

echo "=== numerical verifiers ==="
"$CXX" -std=c++17 -O2 -I"$WORK" "$VERIFY/verify_equilibrium.cpp" -o "$WORK/ve"
"$WORK/ve" | grep -q "max abs error"
echo "ok    equilibrium"
"$CXX" -std=c++17 -O2 -I"$WORK" "$VERIFY/verify_safety.cpp" -o "$WORK/vs"
"$WORK/vs" | grep -q "ALL SAFETY CHECKS PASSED"
echo "ok    safety"

echo "=== csegen smoke ==="
# fixture base name -> a specialization that must appear in the generated output
declare -A CSEGEN_EXPECT=(
  [equilibrium]="SecondOrderImpl<CELL<"
  [force]="ScalarForcePopImpl<"
  [moment]="shearRateMagImpl<"
)
for base in equilibrium force moment; do
  h="$CSEGEN_DIR/$base.h"
  out="$WORK/$base.ur.h"
  "$CSEGEN" "$h" "$out" >/dev/null
  test -s "$out"
  if grep -q "${CSEGEN_EXPECT[$base]}" "$out"; then
    echo "ok    $base.h"
  else
    echo "FAIL  $base.h: missing '${CSEGEN_EXPECT[$base]}'" >&2
    exit 1
  fi
done

if [[ -d "$FREELB/src/lbm" && -f "$FREELB/tools/cse/verify_moment.py" ]]; then
  echo "=== lattice table drift guard ($FREELB) ==="
  python3 "$VERIFY/check_lattice.py" --freelb "$FREELB"

  echo "=== FreeLB verifiers ($FREELB) ==="
  for v in moment equilibrium force; do
    "$CSEGEN" "$FREELB/src/lbm/$v.h" "$WORK/$v.ur.h" >/dev/null
    if out="$(python3 "$FREELB/tools/cse/verify_$v.py" "$FREELB/src/lbm/$v.ur.h" "$WORK/$v.ur.h" 2>&1)"; then
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
