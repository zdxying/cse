#!/usr/bin/env bash
# Engine regression tests. Run via `make test` (binaries must be built).
#
#  1. cost regression : optimize each //@cse fixture and check the post-pass
#                       FLOP count against the expected value. This runs under
#                       three configurations -- default, -r (recombination) and
#                       -s (conservative) -- because several defects only show
#                       up under a specific one. Each stage writes into its own
#                       subdirectory so the generated headers stay separate.
#  2. numerical       : compile/run the verifiers against freshly generated
#                       output, plus the library-level CSEConfig contract test
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

# ---------------------------------------------------------------------------
# 1. cost regression: one stage per configuration.
#    $1 label  $2 extra cse flags  $3 output subdirectory
#    remaining args: fixture=expected-flops
# ---------------------------------------------------------------------------
cost_stage() {
  local label="$1" flags="$2" dir="$3"
  shift 3
  local out="$WORK/$dir"
  mkdir -p "$out"
  echo "=== cost regression ($label) ==="
  local fail=0 pair name want src got
  for pair in "$@"; do
    name="${pair%%=*}"
    want="${pair#*=}"
    src="$out/$name.cpp"
    cp "$FIXTURES/$name.cpp" "$src"
    # A single `-c` run optimizes, writes $src.cse (used by the verifiers below)
    # and reports the cost.
    # shellcheck disable=SC2086
    got="$("$CSE" "$src" $flags -c 2>&1 | sed -n 's/.*After: *\([0-9]*\) flops.*/\1/p')"
    if [[ "$got" == "$want" ]]; then
      echo "ok    $name ($got flops)"
    else
      echo "FAIL  $name: got $got flops, want $want"
      fail=1
    fi
  done
  (( fail == 0 )) || exit 1
}

# The expected counts are "once per statement that mentions the node", not
# "once per node in the DAG": the DAG is hash-consed while codegen prints a
# shared node at every use site, so the per-statement rule is the one that
# tracks the emitted code. `frontend_forms` is the case that shows it -- its
# `counted_postfix` unrolls to four `a = a + b;`, four real additions sharing
# one node.
#
# The numbers below are re-measured under the vector-weighted, unique-node cost
# model merged from origin/main (vector ops cost their lane count; shared DAG
# nodes count once), so several differ from the pre-merge values.
cost_stage "default" "" default \
  basic_cse=10 features=49 namespace_case=4 equilibrium_d3q19=84 \
  safety_cases=19 parens=19 mixed_ops=8 write_visibility=12 \
  effect_duplication=8 frontend_forms=13 comment_braces=4 dead_store_effects=3 constant_edges=6 void_param=4 incdec_safety=12 loop_bound=8 \
  loop_unroll_semantics=12 cost_nested=355 cost_descending=35
cost_stage "-r" "-r" r \
  recombine=24
# Several defects only appear once the aggressive passes are off, so the
# conservative profile needs its own stage.
cost_stage "-s" "-s" s \
  parens=19 store_aware=19 float_identities=6 mixed_ops=9 write_visibility=12 \
  effect_duplication=8 frontend_forms=13 comment_braces=4 \
  dead_store_effects=3 constant_edges=7 void_param=4 incdec_safety=12 loop_bound=8 ref_alias=1 \
  loop_unroll_semantics=12

# The `a*x +/- a` rewrites do not change the FLOP count, so the pinned totals
# above cannot detect their loss; check the generated shape directly.
echo "=== recombination shape checks (-r) ==="
declare -A RECOMBINE_SHAPE=(
  ["a * (1 + x)"]="a*x + a factored"
  ["a * (x - 1)"]="a*x - a factored"
)
shape_fail=0
for pat in "${!RECOMBINE_SHAPE[@]}"; do
  if grep -qF "$pat" "$WORK/r/recombine.cpp.cse"; then
    echo "ok    ${RECOMBINE_SHAPE[$pat]}"
  else
    echo "FAIL  ${RECOMBINE_SHAPE[$pat]}: '$pat' missing from the generated output"
    shape_fail=1
  fi
done
(( shape_fail == 0 )) || exit 1

# `-r` must keep working under `-s`. `-s` clears allowFpReassoc (recombination is
# a floating-point regrouping), so the CLI's explicit `-r` has to carry that
# licence back; otherwise the pass the user asked for is silently dropped.
echo "=== recombination shape check (-s -r) ==="
mkdir -p "$WORK/sr"
cp "$FIXTURES/recombine.cpp" "$WORK/sr/recombine.cpp"
"$CSE" "$WORK/sr/recombine.cpp" -s -r >/dev/null 2>&1
if grep -qF "a * (x + y)" "$WORK/sr/recombine.cpp.cse"; then
  echo "ok    -s -r still recombines"
else
  echo "FAIL  -s -r no longer recombines: the -r licence was dropped" >&2
  exit 1
fi

# `x * 2 -> x + x` must still fire for an operand that may be repeated: the
# guard added for impure operands must not have switched the rewrite off.
echo "=== strength-reduction shape check ==="
if grep -qF "return x + x;" "$WORK/default/effect_duplication.cpp.cse"; then
  echo "ok    x * 2 still strength-reduced"
else
  echo "FAIL  x * 2 is no longer strength-reduced: the purity guard is too broad" >&2
  exit 1
fi

# DCE has to keep a store whose *value* carries an effect even when the target
# is never read, and it must still delete one whose value carries none. Neither
# shows up in the FLOP count (`y++` is not a flop), so check the shape.
echo "=== dead-store shape checks ==="
ds_fail=0
if ! grep -A5 "double inc_in_dead_store" "$WORK/default/dead_store_effects.cpp.cse" \
     | grep -q "unused = y++"; then
  echo "FAIL  a dead store with a side effect was dropped" >&2
  ds_fail=1
fi
if ! grep -A5 "double inc_in_dead_store" "$WORK/default/dead_store_effects.cpp.cse" \
     | grep -q "double unused"; then
  echo "FAIL  the declaration of a kept store was removed with it" >&2
  ds_fail=1
fi
if grep -A5 "double dead_pure_store" "$WORK/default/dead_store_effects.cpp.cse" \
     | grep -q "unused"; then
  echo "FAIL  a dead pure store was kept: DCE no longer fires" >&2
  ds_fail=1
fi
(( ds_fail == 0 )) || exit 1
echo "ok    a dead store is dropped only when its value has no effect"

# The index of a store is an expression slot like any other;
# with a second statement, CSE has to rewrite it on both sides. Counting the
# uses of the extracted variable catches the failure mode where the lvalue is
# collected but never substituted (or substituted only in the value).
# The point of both constant fixes is the spelling that comes out, so grep the
# text: a division by a zero constant must still be a division (it used to be
# folded to 0), and the sign of a zero literal must survive (it used to be
# emitted as the integer `-0`, which is +0.0).
echo "=== constant-emission shape checks ==="
ce_fail=0
if ! grep -q "1.0 / 0.0" "$WORK/default/constant_edges.cpp.cse"; then
  echo "FAIL  a division by a zero constant was folded away" >&2
  ce_fail=1
fi
if ! grep -q -- "-0.0" "$WORK/default/constant_edges.cpp.cse"; then
  echo "FAIL  the sign of a zero literal was lost in emission" >&2
  ce_fail=1
fi
if ! grep -q "1e16" "$WORK/default/constant_edges.cpp.cse"; then
  echo "FAIL  an exponent literal was re-spelled" >&2
  ce_fail=1
fi
# A subscript must be an integer literal -- `feq[1.0]` does not compile. The
# `1.0` written in an expression and the `1` written as a subscript are the same
# node (a constant's text is not part of its identity), so the emitter must not
# print the source spelling in the index slot.
if grep -qE "\[[0-9][0-9]*\.[0-9]" "$WORK/default/equilibrium_d3q19.cpp.cse"; then
  echo "FAIL  a subscript was emitted as a floating literal" >&2
  ce_fail=1
fi
if ! grep -q "feq\[1\]" "$WORK/default/constant_edges.cpp.cse"; then
  echo "FAIL  the shared-literal subscript is no longer an integer" >&2
  ce_fail=1
fi
(( ce_fail == 0 )) || exit 1
echo "ok    zero divisors, zero signs and subscripts survive emission"

echo "=== store-aware shape checks ==="
uses=$(grep -c "buf\[_cse_[0-9]*_[0-9]*\]" "$WORK/s/store_aware.cpp.cse" || true)
if [[ "$uses" -eq 2 ]]; then
  echo "ok    store index rewritten on both sides"
else
  echo "FAIL  store index: expected the extracted variable in 2 places, got $uses" >&2
  exit 1
fi

# A region the frontend cannot read must not cost the user the rest of the file.
# Before this, the first parse error ran into std::terminate, and because the
# output file is written at the very end, *no* file was produced at all.
echo "=== error containment ==="
cat > "$WORK/containment.cpp" <<'EOF'
//@cse
double good(double a, double b) {
    double x = a * b;
    double y = a * b;
    return x + y;
}
//@cse
//@cse
double bad(double a) {
    return a @ 1.0;
}
//@cse
EOF
contain_fail=0
set +e
"$CSE" "$WORK/containment.cpp" > "$WORK/containment.out" 2> "$WORK/containment.err"
contain_rc=$?
set -e
if [[ "$contain_rc" -ne 2 ]]; then
  echo "FAIL  expected exit 2 (one of two regions skipped), got $contain_rc" >&2
  contain_fail=1
fi
if ! grep -q "_cse_[0-9]*_[0-9]*" "$WORK/containment.cpp.cse" 2>/dev/null; then
  echo "FAIL  the readable region was not optimized" >&2
  contain_fail=1
fi
if ! grep -q "a @ 1.0" "$WORK/containment.cpp.cse" 2>/dev/null; then
  echo "FAIL  the unreadable region was not passed through unchanged" >&2
  contain_fail=1
fi
# The diagnostic has to name the file and the line *in it*, not the offset inside
# the region.
if ! grep -q "containment.cpp:10:14" "$WORK/containment.err"; then
  echo "FAIL  the diagnostic does not point at containment.cpp:10:14" >&2
  cat "$WORK/containment.err" >&2
  contain_fail=1
fi
# Every region failing is a different outcome from some failing.
printf '//@cse\ndouble bad(double a) { return a @ 1.0; }\n//@cse\n' \
  > "$WORK/containment_all.cpp"
set +e
"$CSE" "$WORK/containment_all.cpp" > /dev/null 2>&1
contain_rc_all=$?
set -e
if [[ "$contain_rc_all" -ne 3 ]]; then
  echo "FAIL  expected exit 3 when no region could be read, got $contain_rc_all" >&2
  contain_fail=1
fi
if [[ ! -f "$WORK/containment_all.cpp.cse" ]]; then
  echo "FAIL  no output file was written when every region failed" >&2
  contain_fail=1
fi
(( contain_fail == 0 )) || exit 1
echo "ok    a failing region is skipped and passed through, exit 2 / 3 as expected"

# ---------------------------------------------------------------------------
# 2. numerical verifiers.
#    $1 name  $2 verifier source  $3 expected banner  $4 include dir
# ---------------------------------------------------------------------------
run_verifier() {
  local name="$1" src="$2" banner="$3" inc="$4"
  "$CXX" -std=c++17 -O2 -I"$inc" "$VERIFY/$src" -o "$WORK/v_$name"
  if "$WORK/v_$name" | grep -q "$banner"; then
    echo "ok    $name"
  else
    echo "FAIL  $name" >&2
    "$WORK/v_$name" || true
    exit 1
  fi
}

echo "=== numerical verifiers ==="
# Differential regression for the semantic-safety fixes (logical operators,
# codegen parentheses, compound stores, impure-call effects, integer division,
# shadowing, chain merging, signed zero, fractional loop starts). Generated in
# both profiles so the verifier can include each optimized text.
cp "$FIXTURES/semantics_fixes.cpp" "$WORK/default/semantics_fixes.cpp"
"$CSE" "$WORK/default/semantics_fixes.cpp" >/dev/null 2>&1 || true
run_verifier "semantics_fixes" verify_semantics_fixes.cpp \
  "ALL SEMANTICS-FIX CHECKS PASSED" "$WORK/default"
cp "$FIXTURES/semantics_fixes.cpp" "$WORK/s/semantics_fixes.cpp"
"$CSE" -s "$WORK/s/semantics_fixes.cpp" >/dev/null 2>&1 || true
run_verifier "semantics_fixes_safe" verify_semantics_fixes.cpp \
  "ALL SEMANTICS-FIX CHECKS PASSED" "$WORK/s"
# Loop-unroller regressions: a dropped compound operator and a load moved past
# a store. Both keep the FLOP count identical, so only the values catch them.
run_verifier "loop_unroll_semantics" verify_loop_unroll_semantics.cpp \
  "ALL LOOP-UNROLL CHECKS PASSED" "$WORK/default"
run_verifier "loop_unroll_semantics_safe" verify_loop_unroll_semantics.cpp \
  "ALL LOOP-UNROLL CHECKS PASSED" "$WORK/s"
run_verifier "equilibrium" verify_equilibrium.cpp "max abs error" "$WORK/default"
run_verifier "safety" verify_safety.cpp "ALL SAFETY CHECKS PASSED" "$WORK/default"
run_verifier "parens" verify_parens.cpp "ALL PARENS CHECKS PASSED" "$WORK/default"
# These two use a fixed, wrong-shared-node shape, which is only caught by
# comparing values -- the FLOP count is identical either way.
run_verifier "mixed_ops" verify_mixed_ops.cpp "ALL MIXED-OP CHECKS PASSED" \
  "$WORK/default"
run_verifier "write_visibility" verify_write_visibility.cpp \
  "ALL WRITE-VISIBILITY CHECKS PASSED" "$WORK/default"
run_verifier "effect_duplication" verify_effect_duplication.cpp \
  "ALL EFFECT-DUPLICATION CHECKS PASSED" "$WORK/default"
run_verifier "frontend_forms" verify_frontend_forms.cpp \
  "ALL FRONTEND-FORM CHECKS PASSED" "$WORK/default"
# A region whose text holds a brace inside a comment: the extractor must
# take it whole, not cut it off at the comment.
run_verifier "comment_braces" verify_comment_braces.cpp \
  "ALL COMMENT-BRACE CHECKS PASSED" "$WORK/default"
run_verifier "dead_store_effects" verify_dead_store_effects.cpp \
  "ALL DEAD-STORE CHECKS PASSED" "$WORK/default"
run_verifier "constant_edges" verify_constant_edges.cpp \
  "ALL CONSTANT-EDGE CHECKS PASSED" "$WORK/default"
run_verifier "void_param" verify_void_param.cpp \
  "ALL VOID-PARAM CHECKS PASSED" "$WORK/default"
run_verifier "incdec_safety" verify_incdec_safety.cpp \
  "ALL INCDEC-SAFETY CHECKS PASSED" "$WORK/default"
run_verifier "loop_bound" verify_loop_bound.cpp \
  "ALL LOOP-BOUND CHECKS PASSED" "$WORK/default"
# The same fixtures generated by the conservative profile.
run_verifier "parens_safe" verify_parens.cpp "ALL PARENS CHECKS PASSED" "$WORK/s"
run_verifier "store_aware_safe" verify_store_aware.cpp \
  "ALL STORE-AWARE CHECKS PASSED" "$WORK/s"
run_verifier "mixed_ops_safe" verify_mixed_ops.cpp "ALL MIXED-OP CHECKS PASSED" \
  "$WORK/s"
run_verifier "write_visibility_safe" verify_write_visibility.cpp \
  "ALL WRITE-VISIBILITY CHECKS PASSED" "$WORK/s"
run_verifier "effect_duplication_safe" verify_effect_duplication.cpp \
  "ALL EFFECT-DUPLICATION CHECKS PASSED" "$WORK/s"
run_verifier "frontend_forms_safe" verify_frontend_forms.cpp \
  "ALL FRONTEND-FORM CHECKS PASSED" "$WORK/s"
run_verifier "comment_braces_safe" verify_comment_braces.cpp \
  "ALL COMMENT-BRACE CHECKS PASSED" "$WORK/s"
run_verifier "dead_store_effects_safe" verify_dead_store_effects.cpp \
  "ALL DEAD-STORE CHECKS PASSED" "$WORK/s"
run_verifier "constant_edges_safe" verify_constant_edges.cpp \
  "ALL CONSTANT-EDGE CHECKS PASSED" "$WORK/s"
run_verifier "void_param_safe" verify_void_param.cpp \
  "ALL VOID-PARAM CHECKS PASSED" "$WORK/s"
run_verifier "incdec_safety_safe" verify_incdec_safety.cpp \
  "ALL INCDEC-SAFETY CHECKS PASSED" "$WORK/s"
run_verifier "loop_bound_safe" verify_loop_bound.cpp \
  "ALL LOOP-BOUND CHECKS PASSED" "$WORK/s"
run_verifier "ref_alias" verify_ref_alias.cpp \
  "ALL REF-ALIAS CHECKS PASSED" "$WORK/s"
# These identities are only licensed by the default profile, so the conservative
# output is what can be checked numerically.
run_verifier "float_identities" verify_float_identities.cpp \
  "ALL FLOAT-IDENTITY CHECKS PASSED" "$WORK/s"
run_verifier "recombine" verify_recombine.cpp "ALL RECOMBINE CHECKS PASSED" "$WORK/r"

# Library-level CSEConfig contract: the flags added for the unsafe floating-point
# identities and for multiplication regrouping are not reachable from the CLI,
# which only offers "everything on" (the FreeLB profile) or "everything off"
# (-s).
echo "=== config contract ==="
"$CXX" -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_config.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_config"
"$WORK/v_config" | grep -q "ALL CONFIG CHECKS PASSED"
echo "ok    config"

# The builder's guards on empty statement slots are unreachable from the CLI (the
# parser always fills them), so they are driven through the library like the
# config contract above.
echo "=== builder guards ==="
"$CXX" -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_builder_guards.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_builder"
"$WORK/v_builder" | grep -q "ALL BUILDER-GUARD CHECKS PASSED"
echo "ok    builder guards"

# ---------------------------------------------------------------------------
# 3. csegen smoke
# ---------------------------------------------------------------------------
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
  "$CSEGEN" "$h" "$out" >/dev/null 2>"$WORK/$base.gen.err"
  test -s "$out"
  if grep -q "${CSEGEN_EXPECT[$base]}" "$out"; then
    echo "ok    $base.h"
  else
    echo "FAIL  $base.h: missing '${CSEGEN_EXPECT[$base]}'" >&2
    exit 1
  fi
  # Coverage guard: every marked struct must be specialized (0 skipped), and
  # the summary line must be present. A struct silently turning "unsupported"
  # would drop it from the .ur.h with no other test noticing.
  if ! grep -q "coverage: .* 0 skipped" "$WORK/$base.gen.err"; then
    echo "FAIL  $base.h: coverage report missing or shows skips:" >&2
    sed 's/^/      /' "$WORK/$base.gen.err" >&2
    exit 1
  fi
done

# --cost measures the emitted pipeline and must report a total.
"$CSEGEN" --cost --lattice D3Q19 "$CSEGEN_DIR/moment.h" "$WORK/moment_cost.ur.h" \
  >"$WORK/moment_cost.txt"
if grep -q "Total: Before" "$WORK/moment_cost.txt"; then
  echo "ok    csegen --cost"
else
  echo "FAIL  csegen --cost produced no report" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 4. FreeLB (optional)
# ---------------------------------------------------------------------------
if [[ -d "$FREELB/src/lbm" && -f "$FREELB/tools/cse/verify_moment.py" ]]; then
  echo "=== lattice table drift guard ($FREELB) ==="
  python3 "$VERIFY/check_lattice.py" --freelb "$FREELB"

  echo "=== FreeLB verifiers ($FREELB) ==="
  for v in moment equilibrium force; do
    ref="$FREELB/src/lbm/$v.ur.h"
    # The verifier compares the freshly generated header against the pinned
    # reference committed in FreeLB. Without that reference there is nothing to
    # compare against, so skip it (visibly) rather than fail on a missing file.
    if [[ ! -f "$ref" ]]; then
      echo "skip  $v (no pinned reference at $ref)"
      continue
    fi
    "$CSEGEN" "$FREELB/src/lbm/$v.h" "$WORK/$v.ur.h" >/dev/null
    if out="$(python3 "$FREELB/tools/cse/verify_$v.py" "$ref" "$WORK/$v.ur.h" 2>&1)"; then
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
