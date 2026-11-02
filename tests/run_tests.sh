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
# Extra flags threaded into every verifier compile+link (e.g. sanitizers, set by
# `make sanitize`). Kept separate from CXX because CXX is quoted at the call
# sites and so cannot carry a flag list.
EXTRA_CXXFLAGS="${EXTRA_CXXFLAGS:-}"
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

# The expected counts follow the cost model's rule: charge each *reference* to a
# DAG node, not each distinct node. The DAG is hash-consed while codegen prints a
# shared node at every use site, so a shared, unmaterialized node costs once per
# use site; a node CSE materialized is one definition whose uses are cheap
# variable references. `totalNodes` is the separate distinct-node count.
# `frontend_forms` shows the rule -- its `counted_postfix` unrolls to four
# `a = a + b;`, four real additions sharing one node, charged 4.
# tests/verify/verify_cost.cpp pins the rule on shapes where it differs from
# unique-node counting.
#
# The numbers below are re-measured under this rule (vector ops cost their lane
# count; references count per use site), so several differ from the previous
# unique-node values.
cost_stage "default" "" default \
  basic_cse=10 features=51 namespace_case=4 equilibrium_d3q19=84 \
  safety_cases=21 parens=19 mixed_ops=8 write_visibility=12 \
  effect_duplication=13 frontend_forms=16 comment_braces=4 dead_store_effects=3 constant_edges=6 void_param=4 incdec_safety=12 loop_bound=8 \
  loop_unroll_semantics=12 value_prop_chain=6 cleanup_store_index=5 cost_nested=355 cost_descending=35 \
  ref_param_store=9 loop_unroll_inline=39 struct_region=2 region_toplevel=3 semantics_fixes=77 \
  chained_accum=3 cleanup_nested=5 loop_control=6 member_purity=2 const_typing=7 \
  numeric_kind=14 cleanup_chain_effects=8 loop_unroll_chain=42 purity_names=2 \
  zero_product_kind=5
# `pp_directives` is deliberately absent from this table: every one of its
# regions is passed through unchanged, so it has no post-pass FLOP count to pin
# (it reports 0 -> 0). Its oracle is `verify_pp_directives` plus the shape
# checks in "preprocessor directives" below.
cost_stage "-r" "-r" r \
  recombine=24 semantics_fixes=77 effect_order=10

# Several defects only appear once the aggressive passes are off, so the
# conservative profile needs its own stage.
cost_stage "-s" "-s" s \
  parens=19 store_aware=21 float_identities=8 exact_identities=6 mixed_ops=8 \
  write_visibility=12 effect_duplication=13 frontend_forms=16 comment_braces=4 \
  dead_store_effects=3 constant_edges=6 void_param=4 incdec_safety=12 loop_bound=8 ref_alias=1 \
  loop_unroll_semantics=12 value_prop_chain=6 cleanup_store_index=5 \
  ref_param_store=9 loop_unroll_inline=40 struct_region=2 region_toplevel=3 semantics_fixes=76 \
  chained_accum=5 cleanup_nested=5 loop_control=6 member_purity=2 const_typing=7 \
  numeric_kind=16 cleanup_chain_effects=8 loop_unroll_chain=22 speculation=8 loop_escape=15 dce_nested=5 \
  zero_product_kind=12

# The `a*x +/- a` rewrites do not change the FLOP count, so the pinned totals
# above cannot detect their loss; check the generated shape directly.
echo "=== recombination shape checks (-r) ==="
declare -A RECOMBINE_SHAPE=(
  ["a * (1.0 + x)"]="a*x + a factored"
  ["a * (x - 1.0)"]="a*x - a factored"
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

# A loop whose counter is read after the loop must not be unrolled: splicing the
# body out would leave the read naming nothing. The input is not valid C++, so it
# cannot be a compiled fixture -- generate it and assert the `for (` survived.
echo "=== loop-counter escape shape check ==="
cat > "$WORK/escape.cpp" <<'EOF'
//@cse
double f(double a) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) { s = s + a; }
    return s + i;
}
EOF
"$CSE" -s "$WORK/escape.cpp" >/dev/null 2>&1 || true
if grep -q "for (int i = 0; i < 4; ++i)" "$WORK/escape.cpp.cse"; then
  echo "ok    a loop whose counter escapes is left unrolled"
else
  echo "FAIL  a loop whose counter escapes was unrolled (the read now dangles)" >&2
  exit 1
fi

# Restricting the builtin purity whitelist to qualified callees must not
# disable dedup of the real builtins: `std::sqrt` twice still collapses to one
# call. A bare user `max` twice must *not* (that is the verifier's job; this
# check pins the other direction, which no value oracle can see).
echo "=== qualified-builtin purity shape check ==="
sqrt_calls="$(awk '/double std_sqrt_twice/,/^}/' "$WORK/default/purity_names.cpp.cse" |
              grep -c 'std::sqrt(')"
if [[ "$sqrt_calls" == "1" ]]; then
  echo "ok    std::sqrt still deduplicates"
else
  echo "FAIL  std::sqrt is no longer deduplicated ($sqrt_calls calls, want 1)" >&2
  exit 1
fi

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
# ... but an unsigned operand must not be factored: a*x + a is a double
# expression while a*(x+1) wraps x+1 in 32-bit arithmetic.
cp "$FIXTURES/numeric_kind.cpp" "$WORK/sr/numeric_kind.cpp"
"$CSE" "$WORK/sr/numeric_kind.cpp" -s -r >/dev/null 2>&1
if grep -qF "a * (x + 1" "$WORK/sr/numeric_kind.cpp.cse"; then
  echo "FAIL  a*x + a was factored with an unsigned x" >&2
  exit 1
fi
echo "ok    recombination refuses a mixed int/float regrouping"

# `x * 2 -> x + x` must still fire for an operand that may be repeated: the
# guard added for impure operands must not have switched the rewrite off.
echo "=== strength-reduction shape check ==="
if grep -qF "return x + x;" "$WORK/default/effect_duplication.cpp.cse"; then
  echo "ok    x * 2 still strength-reduced"
else
  echo "FAIL  x * 2 is no longer strength-reduced: the purity guard is too broad" >&2
  exit 1
fi

# The exact tier must survive the conservative profile: `-s` disables the
# *assumption* tier, not the bit-exact rewrites. Check a representative sample
# directly -- the tiny fixture's FLOP count cannot distinguish the rules, and a
# regression that re-gated them would otherwise only show up as a value test.
echo "=== exact-tier shape checks (-s) ==="
declare -A EXACT_SHAPE=(
  ["return x;"]="x * 1 -> x"
  ["return a - b;"]="a + (-b) -> a - b"
  ["return a + b;"]="a - (-b) -> a + b"
  ["return x + x;"]="x * 2 -> x + x"
  ["return a * a;"]="(-a)*(-a) -> a*a"
  ["return -(a * b);"]="a * (-b) -> -(a*b)"
)
exact_fail=0
for pat in "${!EXACT_SHAPE[@]}"; do
  if grep -qF "$pat" "$WORK/s/exact_identities.cpp.cse"; then
    echo "ok    -s keeps ${EXACT_SHAPE[$pat]}"
  else
    echo "FAIL  -s dropped ${EXACT_SHAPE[$pat]}: the exact tier was re-gated" >&2
    exact_fail=1
  fi
done
(( exact_fail == 0 )) || exit 1

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

# A store to a *reference* must survive DCE (it writes the caller's object), but
# a store to a pointer parameter's own value must still be dropped (the pointer
# is a local copy). Neither shows up in the FLOP count, so check the text.
echo "=== reference-store shape checks ==="
rs_fail=0
# Pull each optimized function's body out (signature line .. the first column-0
# `}`) so the unmarked `ref_` twin on the following line cannot satisfy a grep.
body() { sed -n "/$1/,/^}/p" "$2"; }
if ! body "double sf_ref_param(" "$WORK/default/ref_param_store.cpp.cse" | grep -q "v = "; then
  echo "FAIL  a store to a reference parameter was deleted" >&2
  rs_fail=1
fi
if ! body "double sf_ref_local(" "$WORK/default/ref_param_store.cpp.cse" | grep -q "r = "; then
  echo "FAIL  a store through a reference local was deleted" >&2
  rs_fail=1
fi
if ! body "int sf_ref_int(" "$WORK/default/ref_param_store.cpp.cse" | grep -q "v = "; then
  echo "FAIL  a store to an integer reference parameter was deleted" >&2
  rs_fail=1
fi
if body "double sf_ptr_self(" "$WORK/default/ref_param_store.cpp.cse" | grep -q "p = 0"; then
  echo "FAIL  a pointer parameter's own store was kept: DCE became too conservative" >&2
  rs_fail=1
fi
(( rs_fail == 0 )) || exit 1
echo "ok    a reference store survives; a pointer self-store is still dead"

# The unroller must rename a body-local whose initializer reads a variable the
# body writes, not inline it (which would move the read past the write).
echo "=== loop-unroll inline shape check ==="
if grep -q "t__u0" "$WORK/default/loop_unroll_inline.cpp.cse"; then
  echo "ok    the written-dependency local is renamed, not inlined"
else
  echo "FAIL  the unroller inlined a local past a write to a variable it reads" >&2
  exit 1
fi

# CleanupPass must fold a chain of additive updates into one initializer -- but
# only under the default profile, which licenses the signed-zero identity the
# merge performs (it drops the accumulator's leading zero). `-s` must keep the
# chain, so this checks the default output. The rewrite is FLOP-neutral, so the
# pinned cost cannot see a regression; check the shape. This is also the fixture
# that keeps the pass-contract oracle (D5) honest -- without it CleanupPass
# fires on nothing in the suite.
# The fixture's own comments quote the shapes being checked, so strip full-line
# comments before grepping -- otherwise the check matches the documentation.
echo "=== cleanup chain shape check ==="
default_chain="$(grep -v '^[[:space:]]*//' "$WORK/default/chained_accum.cpp.cse" || true)"
if grep -q "double x = a + b + c;" <<<"$default_chain" &&
   grep -q "acc = a + b;" <<<"$default_chain"; then
  echo "ok    chained additive updates are combined into one initializer"
else
  echo "FAIL  CleanupPass no longer combines a chained additive update" >&2
  exit 1
fi

# ... and it must NOT do so under `-s`: the merge drops the accumulator's
# leading zero, which is the same signed-zero identity `x + 0 -> x`. `-s` must
# keep `x = 0; x = x + a; ...` so a zero result keeps its sign.
echo "=== cleanup chain conservatism check (-s) ==="
s_chain="$(grep -v '^[[:space:]]*//' "$WORK/s/chained_accum.cpp.cse" || true)"
if grep -q "x = x + a;" <<<"$s_chain" &&
   ! grep -q "double x = a + b" <<<"$s_chain"; then
  echo "ok    -s keeps the chained accumulator (signed-zero identity gated off)"
else
  echo "FAIL  -s folded a chained accumulator: the signed-zero licence leaked" >&2
  sed 's/^/      /' "$WORK/s/chained_accum.cpp.cse" >&2
  exit 1
fi

# CleanupPass must now recurse into nested blocks: a dead declaration or a
# self-assignment inside an `if` / `for` body used to survive, because
# `pruneBlock` only ever pruned the top-level function-body block. The `ref_`
# twins keep the dead code on purpose, so scope each grep to the optimized
# `sf_` body with the `body` helper.
echo "=== cleanup nested-block shape checks ==="
cn="$WORK/default/cleanup_nested.cpp.cse"
nested_fail=0
if body "double sf_nested_if(" "$cn" | grep -q "dead"; then
  echo "FAIL  a dead declaration inside an if branch survived" >&2
  nested_fail=1
fi
if body "double sf_nested_if(" "$cn" | grep -q "a\[0\] = a\[0\]"; then
  echo "FAIL  a self-assignment inside an if branch survived" >&2
  nested_fail=1
fi
if body "double sf_nested_for(" "$cn" | grep -q "dead"; then
  echo "FAIL  a dead declaration inside a for body survived" >&2
  nested_fail=1
fi
if body "double sf_nested_for(" "$cn" | grep -q "a\[i\] = a\[i\]"; then
  echo "FAIL  a self-assignment inside a for body survived" >&2
  nested_fail=1
fi
if ! body "double sf_nested_used(" "$cn" | grep -q "double t ="; then
  echo "FAIL  a live declaration inside an if branch was deleted" >&2
  nested_fail=1
fi
(( nested_fail == 0 )) || exit 1
echo "ok    nested dead code is removed; a live nested declaration survives"

# `break;` / `continue;` are spelled as a bare-variable expression statement in
# this IR, so a CleanupPass that erases "useless" variable statements deletes
# them. The regression only reached a *braced* block; the shape check pins the
# keyword directly, and the twin differential below pins the values.
# The chained-add merge must not fuse an impure addend or an aliasing reference
# parameter. Both hazards keep the FLOP count and the effect *count* identical,
# so only the shape (and the twin values) can see them.
echo "=== cleanup-chain shape checks ==="
cc="$WORK/default/cleanup_chain_effects.cpp.cse"
cc_fail=0
if grep -q "++y + ++y" "$cc"; then
  echo "FAIL  two sequenced increments were fused into an unsequenced expression" >&2
  cc_fail=1
fi
if grep -q "x = q + 2.0" "$cc"; then
  echo "FAIL  a chain whose addend aliases the accumulator was merged" >&2
  cc_fail=1
fi
(( cc_fail == 0 )) || exit 1
echo "ok    impure and aliasing chains are left sequenced"

echo "=== loop-control shape checks ==="
lc="$WORK/default/loop_control.cpp.cse"
lc_fail=0
for fn in sf_break_braced sf_continue_braced sf_break_nested; do
  case "$fn" in
    *continue*) kw="continue;";;
    *)          kw="break;";;
  esac
  if ! body "double $fn(" "$lc" | grep -q "$kw"; then
    echo "FAIL  $kw was erased from $fn" >&2
    lc_fail=1
  fi
done
(( lc_fail == 0 )) || exit 1
echo "ok    break/continue survive in braced loop bodies"

# Deeply nested expressions used to overflow the parser's stack: `bin/cse` died
# with SIGSEGV and wrote no output at all. The nesting limit turns that into a
# normal unreadable region -- a diagnostic, a passthrough, and exit code 3.
echo "=== expression depth limit ==="
python3 - "$WORK/deep.cpp" <<'PYEOF'
import sys
n = 5000
e = "a"
for _ in range(n):
    e = "(" + e + " + a)"
open(sys.argv[1], "w").write("//@cse\ndouble f(double a) {\n  return " + e + ";\n}\n")
PYEOF
deep_rc=0
"$CSE" -s "$WORK/deep.cpp" >/dev/null 2>"$WORK/deep.err" || deep_rc=$?
if [[ "$deep_rc" == "3" ]] && grep -q "CSE-PARSE-DEPTH" "$WORK/deep.err" &&
   [[ -f "$WORK/deep.cpp.cse" ]]; then
  echo "ok    a pathologically nested expression is diagnosed, not a crash"
else
  echo "FAIL  deep expression: rc=$deep_rc (want 3 with CSE-PARSE-DEPTH and an output file)" >&2
  sed 's/^/      /' "$WORK/deep.err" | head -3 >&2
  exit 1
fi
# The limit must not reject realistic code: 100 levels still optimizes.
# The limit must not reject realistic code. 100 levels parses and optimizes:
# `((a*a)*a)*...` collapses to a flat product, so the assertion is "the region
# was optimized, not passed through" -- checking for a `_cse_` temporary would
# be wrong, since a chain over one variable has nothing to share.
python3 - "$WORK/deep_ok.cpp" <<'PYEOF'
import sys
e = "a"
for _ in range(100):
    e = "(" + e + " * a)"
open(sys.argv[1], "w").write("//@cse\ndouble f(double a) {\n  return " + e + ";\n}\n")
PYEOF
deep_ok_rc=0
"$CSE" -s "$WORK/deep_ok.cpp" >/dev/null 2>"$WORK/deep_ok.err" || deep_ok_rc=$?
if [[ "$deep_ok_rc" == "0" ]] &&
   grep -q "1 optimized" "$WORK/deep_ok.err"; then
  echo "ok    100 levels of nesting still optimizes"
else
  echo "FAIL  100 levels of nesting was rejected (rc=$deep_ok_rc, the limit is too low)" >&2
  sed 's/^/      /' "$WORK/deep_ok.err" | head -3 >&2
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
  local name="$1" src="$2" banner="$3" inc="$4" extra="${5:-}"
  # shellcheck disable=SC2086
  "$CXX" $EXTRA_CXXFLAGS $extra -std=c++17 -O2 -I"$inc" "$VERIFY/$src" -o "$WORK/v_$name"
  # The exit code is authoritative; the banner only proves the run reached the
  # end. Grepping the banner alone let a verifier that printed it unconditionally
  # (verify_equilibrium did) pass even when it reported a failure. `|| rc=$?`
  # keeps `set -e` from aborting before the diagnostic below is printed.
  local out rc=0
  out="$("$WORK/v_$name")" || rc=$?
  if [[ $rc -eq 0 ]] && grep -q "$banner" <<<"$out"; then
    echo "ok    $name"
  else
    echo "FAIL  $name (exit $rc)" >&2
    printf '%s\n' "$out" >&2
    exit 1
  fi
}

# run_verifier_for <src> <banner> <profile> [name]
#   Derives the display name (verify_X.cpp -> X, or X_safe for the conservative
#   profile) and the include dir from the source + profile, so a call site only
#   names the verifier and the profile. `name` overrides the derived one (used by
#   the verifiers that only run under -s and so keep their bare name). The
#   profile dirs are the ones the matching cost_stage wrote the generated
#   `*.cpp.cse` headers into.
run_verifier_for() {
  local src="$1" banner="$2" prof="$3" explicit="${4:-}"
  local stem="${src#verify_}"
  stem="${stem%.cpp}"
  local dir name extra=""
  case "$prof" in
    default) dir="$WORK/default"; name="$stem" ;;
    # -s licenses no floating-point reassociation, so an optimized twin must be
    # bit-exact: tell the harness to tighten checkNear.
    s)       dir="$WORK/s";       name="${stem}_safe"; extra="-DCSE_CONSERVATIVE" ;;
    r)       dir="$WORK/r";       name="$stem" ;;
    *) echo "run_verifier_for: unknown profile '$prof'" >&2; exit 2 ;;
  esac
  [[ -n "$explicit" ]] && name="$explicit"
  run_verifier "$name" "$src" "$banner" "$dir" "$extra"
}

echo "=== numerical verifiers ==="
# Differential regression for the semantic-safety fixes (logical operators,
# codegen parentheses, compound stores, impure-call effects, integer division,
# shadowing, chain merging, signed zero, fractional loop starts). Generated in
# both profiles so the verifier can include each optimized text.
cp "$FIXTURES/semantics_fixes.cpp" "$WORK/default/semantics_fixes.cpp"
"$CSE" "$WORK/default/semantics_fixes.cpp" >/dev/null 2>&1 || true
run_verifier_for verify_semantics_fixes.cpp "ALL SEMANTICS-FIX CHECKS PASSED" default
cp "$FIXTURES/semantics_fixes.cpp" "$WORK/s/semantics_fixes.cpp"
"$CSE" -s "$WORK/s/semantics_fixes.cpp" >/dev/null 2>&1 || true
run_verifier_for verify_semantics_fixes.cpp "ALL SEMANTICS-FIX CHECKS PASSED" s
# Case 25 of semantics_fixes is a `<` comparison followed by a `>`: when the
# template scanner mis-read the `<`, the region failed to parse and was passed
# through unchanged -- still correct, so the value checks above cannot see it.
# Assert the region was actually optimized (CSE extracted the shared `a * b`).
echo "=== comparison-parse shape check ==="
if body "double sf_lt_gt(" "$WORK/default/semantics_fixes.cpp.cse" | grep -q "_cse_"; then
  echo "ok    a region with '<' then '>' is still optimized"
else
  echo "FAIL  a region with '<' then '>' was passed through: the parser dropped it" >&2
  exit 1
fi
# Loop-unroller regressions: a dropped compound operator and a load moved past
# a store. Both keep the FLOP count identical, so only the values catch them.
run_verifier_for verify_loop_unroll_semantics.cpp "ALL LOOP-UNROLL CHECKS PASSED" default
run_verifier_for verify_loop_unroll_semantics.cpp "ALL LOOP-UNROLL CHECKS PASSED" s
# Value-propagation regressions: a chain of trivial locals used to leave a
# dangling reference. The plain cases fail to compile; the shadow case compiles
# and silently reads the wrong variable, so only the values catch it.
run_verifier_for verify_loop_unroll_chain.cpp "ALL LOOP-UNROLL-CHAIN CHECKS PASSED" default
run_verifier_for verify_loop_unroll_chain.cpp "ALL LOOP-UNROLL-CHAIN CHECKS PASSED" s

# Convergence, not a round cap. Long_chain is a 60-link definition chain -- far
# past the 4/5/10 magic numbers that ValueProp, DCE and Cleanup used to stop at.
# The value half is below; the *shape* half is here, because a chain that stops
# halfway still computes the right number, so no value oracle can see it. What
# changes is that the emitted body keeps `double vN = ...;` declarations and a
# second `bin/cse` run finishes the job.
echo "=== pass convergence (no round caps) ==="
conv_fail=0
for prof in default s; do
  mkdir -p "$WORK/$prof"
  cp "$FIXTURES/long_chain.cpp" "$WORK/$prof/long_chain.cpp"
  if [[ "$prof" == s ]]; then
    "$CSE" -s "$WORK/$prof/long_chain.cpp" >/dev/null 2>&1 || true
  else
    "$CSE" "$WORK/$prof/long_chain.cpp" >/dev/null 2>&1 || true
  fi
  # The optimized region must have collapsed to the identity function: every
  # link inlined, no residue declarations, the doubling folded to `a + a`.
  lc_body="$(sed -n '/double sf_chain_60/,/^}/p' "$WORK/$prof/long_chain.cpp.cse")"
  residue=$(grep -c 'double v' <<<"$lc_body" || true)
  if [[ "$residue" -ne 0 ]]; then
    echo "FAIL  [$prof] long_chain left $residue decl(s): a pass stopped at a cap" >&2
    printf '%s\n' "$lc_body" | sed 's/^/      /' >&2
    conv_fail=1
  fi
  # A second run must be a no-op on the emitted file: truncation shows up as
  # "the same input produces more output the second time".
  cp "$WORK/$prof/long_chain.cpp.cse" "$WORK/$prof/long_chain.run2.cpp"
  if [[ "$prof" == s ]]; then
    "$CSE" -s "$WORK/$prof/long_chain.run2.cpp" >/dev/null 2>&1 || true
  else
    "$CSE" "$WORK/$prof/long_chain.run2.cpp" >/dev/null 2>&1 || true
  fi
  if ! diff -q <(sed -n '/double sf_chain_60/,/^}/p' "$WORK/$prof/long_chain.cpp.cse") \
               <(sed -n '/double sf_chain_60/,/^}/p' "$WORK/$prof/long_chain.run2.cpp.cse") \
       >/dev/null 2>&1; then
    echo "FAIL  [$prof] the pipeline is not a fixpoint on our own output" >&2
    sed -n '/double sf_chain_60/,/^}/p' "$WORK/$prof/long_chain.run2.cpp.cse" | sed 's/^/      /' >&2
    conv_fail=1
  fi
done
(( conv_fail == 0 )) || exit 1
echo "ok    a 60-link chain converges; a second run changes nothing"
run_verifier_for verify_long_chain.cpp "ALL LONG-CHAIN CHECKS PASSED" default
run_verifier_for verify_long_chain.cpp "ALL LONG-CHAIN CHECKS PASSED" s

# A numeric kind must survive LoopUnroll's per-iteration renaming. `numKind` is
# derived metadata attached when the builder sees a declaration, so a renamed
# body-local used to stay Unknown -- and `v / v` folded to `1.0` outside a loop
# but not inside one. Value checks cannot see that (both compute the same
# number); the shape check is the discriminating one.
echo "=== numeric kind through loop unrolling ==="
kind_fail=0
for prof in default s; do
  mkdir -p "$WORK/$prof"
  cp "$FIXTURES/kind_through_unroll.cpp" "$WORK/$prof/kind_through_unroll.cpp"
  if [[ "$prof" == s ]]; then
    "$CSE" -s "$WORK/$prof/kind_through_unroll.cpp" >/dev/null 2>&1 || true
  else
    "$CSE" "$WORK/$prof/kind_through_unroll.cpp" >/dev/null 2>&1 || true
  fi
  # The shape assertion is default-only: `x / x -> 1` is one of the field
  # identities that `-s` deliberately withholds (allowUnsafeFpIdentities), so a
  # surviving division there is correct, not a lost kind. The value verifier
  # below still runs under both profiles.
  if [[ "$prof" == default ]]; then
    body="$(sed -n '/double sf_kind_through_unroll/,/^}/p' \
              "$WORK/$prof/kind_through_unroll.cpp.cse")"
    # The unrolled body must have folded `v__uN / v__uN` to 1.0. A leftover
    # division means the renamed variable lost its Float kind.
    if grep -q ' / ' <<<"$body"; then
      echo "FAIL  [$prof] a renamed local lost its numeric kind (division survived)" >&2
      printf '%s\n' "$body" | sed 's/^/      /' >&2
      kind_fail=1
    fi
  fi
done
(( kind_fail == 0 )) || exit 1
echo "ok    a renamed local keeps its kind; x/x still folds inside an unrolled loop"
run_verifier_for verify_kind_through_unroll.cpp \
  "ALL KIND-THROUGH-UNROLL CHECKS PASSED" default
run_verifier_for verify_kind_through_unroll.cpp \
  "ALL KIND-THROUGH-UNROLL CHECKS PASSED" s

# Reassociation may not move an effectful term past a read of what it writes.
# The pass sorts an additive chain by use frequency, which reorders evaluation;
# with an impure call in the chain that is a silent miscompile
# (`state + f() + state` -> `state + state + f()`), invisible to the
# effect-preservation oracle because the effect multiset is unchanged.
#
# The verifier runs under -r (where the pass is wired in) and the shape check
# asserts both directions: the effectful chain keeps its source order, while an
# effect-free chain is still reordered (so "fix" it by disabling the pass fails).
echo "=== effect order under reassociation (-r) ==="
eo_fail=0
# cost_stage "-r" above already wrote $WORK/r/effect_order.cpp.cse.
eo_shared="$WORK/r/effect_order.cpp.cse"
for fn in sf_effect_order_0 sf_effect_order_1 sf_effect_order_2; do
  body="$(sed -n "/double $fn/,/^}/p" "$eo_shared")"
  # The impure call must not appear before a read that follows it in source.
  # For _0/_1 the call is written after/before the reads respectively; in both
  # cases the call must stay adjacent to its source position, which the value
  # verifier checks. What we can assert textually is that the emitted chain is
  # not the frequency-sorted one (`state_probe + state_probe` before the call).
  if grep -q 'state_probe + state_probe + bump_probe' <<<"$body"; then
    echo "FAIL  $fn: a pure read was sorted ahead of the impure call" >&2
    printf '%s\n' "$body" | sed 's/^/      /' >&2
    eo_fail=1
  fi
done
# Control: the effect-free chain must STILL be reassociated, otherwise the pass
# has been disabled rather than made safe.
pure_body="$(sed -n '/double sf_pure_chain/,/^}/p' "$eo_shared")"
if ! grep -q 'a + a + b + c' <<<"$pure_body"; then
  echo "FAIL  an effect-free additive chain is no longer reassociated" >&2
  printf '%s\n' "$pure_body" | sed 's/^/      /' >&2
  eo_fail=1
fi
(( eo_fail == 0 )) || exit 1
echo "ok    effects keep their order; effect-free chains still reassociate"
run_verifier_for verify_effect_order.cpp "ALL EFFECT-ORDER CHECKS PASSED" r

# Four frontend shapes that used to make a whole region unparseable -- `size_t`
# as a for-init type, top-level `static inline`, `<<`/`>>`, and a dereference.
# None produced wrong code (the region was skipped), but the region's
# optimizations were silently dropped, and a value check cannot tell "optimized"
# from "passed through". The shape assertions below are the discriminating half.
echo "=== frontend gap shapes are optimized ==="
fg_fail=0
for prof in default s; do
  mkdir -p "$WORK/$prof"
  cp "$FIXTURES/frontend_gaps.cpp" "$WORK/$prof/frontend_gaps.cpp"
  if [[ "$prof" == s ]]; then
    "$CSE" -s "$WORK/$prof/frontend_gaps.cpp" >/dev/null 2>&1 || true
  else
    "$CSE" "$WORK/$prof/frontend_gaps.cpp" >/dev/null 2>&1 || true
  fi
  fg_out="$WORK/$prof/frontend_gaps.cpp.cse"
  # The sizet loop must be unrolled (a region that only parses but does not
  # optimize would keep the for-loop shape). Default folds the four adds into
  # one expression; -s keeps one add per iteration. Either way the loop itself
  # must be gone.
  if grep -q 'for (size_t' "$fg_out"; then
    echo "FAIL  [$prof] a size_t for-init loop was not unrolled (the parser still rejects it)" >&2
    fg_fail=1
  fi
  # The static inline region must be optimized, and its modifier preserved.
  if ! grep -q 'static inline double sf_static_inline' "$fg_out"; then
    echo "FAIL  [$prof] the 'static inline' modifier was dropped from the emission" >&2
    fg_fail=1
  fi
  if ! sed -n '/double sf_static_inline/,/^}/p' "$fg_out" | grep -q '_cse_'; then
    echo "FAIL  [$prof] the 'static inline' region was not optimized" >&2
    fg_fail=1
  fi
  # Shifts must survive as shifts (a mis-tokenized `>>` would corrupt the
  # template closers in the same file and show up as a skip).
  grep -q '1 << k' "$fg_out" ||
    { echo "FAIL  [$prof] '1 << k' did not survive to the output" >&2; fg_fail=1; }
  grep -q '1 << k + 1' "$fg_out" ||
    { echo "FAIL  [$prof] shift precedence changed: '1 << k + 1' is not emitted as written" >&2; fg_fail=1; }
  # The deref store must be lowered to p[0] =, not dropped.
  grep -q 'q\[0\] = 2.0' "$fg_out" ||
    { echo "FAIL  [$prof] a store through a dereference was lost" >&2; fg_fail=1; }
done
# The optimized count must be all six regions (a skipped region is the old bug).
cov=$(./bin/cse "$FIXTURES/frontend_gaps.cpp" 2>&1 | grep -oE '[0-9]+ optimized' | head -1 | grep -oE '[0-9]+')
[[ "$cov" == 14 ]] || { echo "FAIL  only $cov/14 regions optimized (a frontend gap reappeared)" >&2; fg_fail=1; }
(( fg_fail == 0 )) || exit 1
echo "ok    size_t/static-inline/shift/deref regions parse AND optimize"
run_verifier_for verify_frontend_gaps.cpp "ALL FRONTEND-GAPS CHECKS PASSED" default
run_verifier_for verify_frontend_gaps.cpp "ALL FRONTEND-GAPS CHECKS PASSED" s
run_verifier_for verify_cleanup_chain_effects.cpp "ALL CLEANUP-CHAIN CHECKS PASSED" default
run_verifier_for verify_cleanup_chain_effects.cpp "ALL CLEANUP-CHAIN CHECKS PASSED" s
run_verifier_for verify_numeric_kind.cpp "ALL NUMERIC-KIND CHECKS PASSED" default
run_verifier_for verify_numeric_kind.cpp "ALL NUMERIC-KIND CHECKS PASSED" s
# The zero identities substitute a constant, so the constant must carry the
# expression's kind. Only the values catch it: folding to the wrong kind leaves
# the FLOP count identical and both spellings parse.
run_verifier_for verify_zero_product_kind.cpp "ALL ZERO-PRODUCT-KIND CHECKS PASSED" default
run_verifier_for verify_zero_product_kind.cpp "ALL ZERO-PRODUCT-KIND CHECKS PASSED" s
run_verifier_for verify_value_prop_chain.cpp "ALL VALUE-PROP-CHAIN CHECKS PASSED" default
run_verifier_for verify_value_prop_chain.cpp "ALL VALUE-PROP-CHAIN CHECKS PASSED" s
# CleanupPass regressions: a local read only as a store index used to have its
# declaration deleted. The plain cases fail to compile; the shadow case compiles
# and writes the wrong slot, so only the values catch it.
run_verifier_for verify_cleanup_store_index.cpp "ALL CLEANUP-STORE-INDEX CHECKS PASSED" default
run_verifier_for verify_cleanup_store_index.cpp "ALL CLEANUP-STORE-INDEX CHECKS PASSED" s
# CleanupPass now recurses into nested blocks. Value-preserving, so the twin
# check proves the recursion does not delete something live; the removal itself
# is pinned by the shape checks above.
run_verifier_for verify_cleanup_nested.cpp "ALL CLEANUP-NESTED CHECKS PASSED" default
run_verifier_for verify_cleanup_nested.cpp "ALL CLEANUP-NESTED CHECKS PASSED" s
# DCE used to delete a store to a reference (a parameter or a reference local),
# because every parameter and declaration counted as private storage. The plain
# cases fail to compile only when the whole function is empty; the rest are
# caught by comparing the mutated referent.
run_verifier_for verify_ref_param_store.cpp "ALL REF-PARAM-STORE CHECKS PASSED" default
run_verifier_for verify_ref_param_store.cpp "ALL REF-PARAM-STORE CHECKS PASSED" s
# The unroller used to inline a body-local initializer past a write to a variable
# it reads. FLOP-neutral, so only the values catch it.
run_verifier_for verify_loop_unroll_inline.cpp "ALL LOOP-UNROLL-INLINE CHECKS PASSED" default
run_verifier_for verify_loop_unroll_inline.cpp "ALL LOOP-UNROLL-INLINE CHECKS PASSED" s
# A region that marks a pure data struct (or a namespace holding only one) used
# to emit nothing; the verifier uses each type, so a dropped definition fails to
# compile.
run_verifier_for verify_struct_region.cpp "ALL STRUCT-REGION CHECKS PASSED" default
# A top-level declaration marked with `//@cse` used to be swallowed into the next
# function's region and dropped; the verifier reads each declaration.
run_verifier_for verify_region_toplevel.cpp "ALL REGION-TOPLEVEL CHECKS PASSED" default
run_verifier_for verify_equilibrium.cpp "max abs error" default
run_verifier_for verify_safety.cpp "ALL SAFETY CHECKS PASSED" default
run_verifier_for verify_parens.cpp "ALL PARENS CHECKS PASSED" default
# These two use a fixed, wrong-shared-node shape, which is only caught by
# comparing values -- the FLOP count is identical either way.
run_verifier_for verify_mixed_ops.cpp "ALL MIXED-OP CHECKS PASSED" default
run_verifier_for verify_write_visibility.cpp "ALL WRITE-VISIBILITY CHECKS PASSED" default
run_verifier_for verify_effect_duplication.cpp "ALL EFFECT-DUPLICATION CHECKS PASSED" default
# A user method named after a builtin math function must not be treated as pure.
run_verifier_for verify_member_purity.cpp "ALL MEMBER-PURITY CHECKS PASSED" default
# ...and neither must a user-defined *free* function of the same name. The
# whitelist now requires a qualified callee (`std::sqrt`), because a bare `max`
# is exactly what user code defines. The verifier counts calls (the value alone
# cannot see a merged pair) and run_tests.sh asserts `std::sqrt` still dedupes.
run_verifier_for verify_purity_names.cpp "ALL PURITY-NAMES CHECKS PASSED" default
run_verifier_for verify_frontend_forms.cpp "ALL FRONTEND-FORM CHECKS PASSED" default
# A region whose text holds a brace inside a comment: the extractor must
# take it whole, not cut it off at the comment.
run_verifier_for verify_comment_braces.cpp "ALL COMMENT-BRACE CHECKS PASSED" default
run_verifier_for verify_dead_store_effects.cpp "ALL DEAD-STORE CHECKS PASSED" default
run_verifier_for verify_constant_edges.cpp "ALL CONSTANT-EDGE CHECKS PASSED" default
run_verifier_for verify_void_param.cpp "ALL VOID-PARAM CHECKS PASSED" default
run_verifier_for verify_incdec_safety.cpp "ALL INCDEC-SAFETY CHECKS PASSED" default
run_verifier_for verify_loop_bound.cpp "ALL LOOP-BOUND CHECKS PASSED" default
# The descending/stepped loop-cost fixture: its loops survive verbatim, so only
# a value oracle (not the FLOP pin alone) can catch a future pass that rewrites
# or collapses them.
run_verifier_for verify_cost_descending.cpp "ALL COST-DESCENDING CHECKS PASSED" default
# The same fixtures generated by the conservative profile.
run_verifier_for verify_parens.cpp "ALL PARENS CHECKS PASSED" s
run_verifier_for verify_store_aware.cpp "ALL STORE-AWARE CHECKS PASSED" s
run_verifier_for verify_mixed_ops.cpp "ALL MIXED-OP CHECKS PASSED" s
run_verifier_for verify_write_visibility.cpp "ALL WRITE-VISIBILITY CHECKS PASSED" s
run_verifier_for verify_effect_duplication.cpp "ALL EFFECT-DUPLICATION CHECKS PASSED" s
run_verifier_for verify_frontend_forms.cpp "ALL FRONTEND-FORM CHECKS PASSED" s
run_verifier_for verify_comment_braces.cpp "ALL COMMENT-BRACE CHECKS PASSED" s
run_verifier_for verify_dead_store_effects.cpp "ALL DEAD-STORE CHECKS PASSED" s
run_verifier_for verify_constant_edges.cpp "ALL CONSTANT-EDGE CHECKS PASSED" s
run_verifier_for verify_void_param.cpp "ALL VOID-PARAM CHECKS PASSED" s
run_verifier_for verify_incdec_safety.cpp "ALL INCDEC-SAFETY CHECKS PASSED" s
run_verifier_for verify_loop_bound.cpp "ALL LOOP-BOUND CHECKS PASSED" s
run_verifier_for verify_ref_alias.cpp "ALL REF-ALIAS CHECKS PASSED" s ref_alias
# These identities are only licensed by the default profile, so the conservative
# output is what can be checked numerically.
run_verifier_for verify_float_identities.cpp "ALL FLOAT-IDENTITY CHECKS PASSED" s float_identities
# The exact tier is *not* disabled under -s: its bit-exact rewrites still fire.
# Pinned numerically (including the sign of zero for `x - 0`) here, and by the
# shape checks above.
run_verifier_for verify_exact_identities.cpp "ALL EXACT-IDENTITY CHECKS PASSED" s exact_identities
run_verifier_for verify_recombine.cpp "ALL RECOMBINE CHECKS PASSED" r

# The preprocessor-directive contract. A `#` line is outside the modelled
# language: the lexer emits it as a token, the extractor flags the region, and
# the driver passes the region through verbatim (regenerating from the AST would
# delete the guard and resurrect `#if 0`). Both profiles are exercised because
# the passthrough must not depend on which algebraic licences are on.
echo "=== preprocessor directives ==="
pp_fail=0
for prof in default s; do
  mkdir -p "$WORK/$prof"
  cp "$FIXTURES/pp_directives.cpp" "$WORK/$prof/pp_directives.cpp"
  if [[ "$prof" == s ]]; then
    "$CSE" -s "$WORK/$prof/pp_directives.cpp" \
      >/dev/null 2>"$WORK/$prof-pp.err" || true
  else
    "$CSE" "$WORK/$prof/pp_directives.cpp" \
      >/dev/null 2>"$WORK/$prof-pp.err" || true
  fi
  run_verifier_for verify_pp_directives.cpp "ALL PP-DIRECTIVES CHECKS PASSED" \
    "$prof" "$( [[ "$prof" == s ]] && echo pp_directives_safe || echo pp_directives )"
  pp_out="$WORK/$prof/pp_directives.cpp.cse"
  # Every directive survives, byte for byte.
  for want in "#if 0" "#endif" "#if 1" "/* note */ #if 0"; do
    grep -qF "$want" "$pp_out" ||
      { echo "FAIL  [$prof] directive '$want' was dropped" >&2; pp_fail=1; }
  done
  # The region is emitted once, verbatim: the two guarded regions keep their
  # pristine `return -1.0;` bodies (one per sf_/ref_ pair member), and no guarded
  # body is duplicated by an AST round-trip. Counting is what catches resurrection
  # -- an `#if 0` body becoming live would add a statement. Anchor on the exact
  # indented statement so the doc comment above is not counted.
  got=$(grep -cF '  return -1.0;' "$pp_out")
  if [[ "$got" -ne 4 ]]; then
    echo "FAIL  [$prof] expected 4 guarded bodies, got $got (resurrection?)" >&2
    pp_fail=1
  fi
  # The three directive regions must be reported as skipped-unsupported; a
  # passthrough that still claims "optimized" is how this hid before.
  if ! grep -q "3 skipped-unsupported" "$WORK/$prof-pp.err"; then
    echo "FAIL  [$prof] directive regions are not reported as skipped-unsupported" >&2
    sed 's/^/      /' "$WORK/$prof-pp.err" >&2
    pp_fail=1
  fi
  # The mirror image: a region whose only '#' sits inside a comment is ordinary
  # code and must still be optimized. A raw line scan refused it.
  if ! grep -q "_cse_" "$pp_out"; then
    echo "FAIL  [$prof] a region whose only '#' is inside a comment was passed through" >&2
    pp_fail=1
  fi
done
(( pp_fail == 0 )) || exit 1
echo "ok    directives survive; comment-only regions stay optimizable"

# Library-level CSEConfig contract: the flags added for the unsafe floating-point
# identities and for multiplication regrouping are not reachable from the CLI,
# which only offers "everything on" (the FreeLB profile) or "everything off"
# (-s).
echo "=== config contract ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_config.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_config"
"$WORK/v_config" | grep -q "ALL CONFIG CHECKS PASSED"
echo "ok    config"

# CounterPropPass is only injected as the post-algebra pass by the FreeLB
# emitter, never by createDefault, so `bin/cse` cannot reach it -- its nested-
# scope seeding is driven through the library like the config contract above.
# The discriminating case (a loop that writes its own counter) folds to a wrong
# index without the entry-seed fix; the rest pin that it does not over-correct.
echo "=== counter-prop nested scopes ==="
# counter_prop lives in the FreeLB plugin tree (its only consumer), so the
# verifier compiles against both roots.
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/plugins/freelb" \
  "$VERIFY/verify_counter_prop.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_counter_prop"
"$WORK/v_counter_prop" | grep -q "ALL COUNTER-PROP CHECKS PASSED"
echo "ok    counter-prop"

# The builder's guards on empty statement slots are unreachable from the CLI (the
# parser always fills them), so they are driven through the library like the
# config contract above.
echo "=== builder guards ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_builder_guards.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_builder"
"$WORK/v_builder" | grep -q "ALL BUILDER-GUARD CHECKS PASSED"
echo "ok    builder guards"

# "Did the rewrite fire?" — the half the value verifiers cannot see. A rewrite
# that simply does not fire leaves the value correct, which is how the FreeLB
# kernels lost half their CSE while every numerical check stayed green. Driven
# through the library so both `assumeFloatOperands` profiles are reachable.
echo "=== zero-product activity ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" \
  "$VERIFY/verify_zero_product_activity.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_zero_activity"
"$WORK/v_zero_activity" | grep -q "ALL ZERO-PRODUCT-ACTIVITY CHECKS PASSED"
echo "ok    zero-product identities fire only where licensed"

# The cost model's counting rule (per reference occurrence, not per unique node)
# is pinned directly, because it has flipped silently before and the FLOP pins
# below can only catch a regression for shapes the fixtures happen to contain.
echo "=== cost-model counting rule ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_cost.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_cost"
"$WORK/v_cost" | grep -q "ALL COST-MODEL CHECKS PASSED"
echo "ok    cost model"

# The write-set analysis (ir/location.{h,cpp}) is the single authority every
# pass consults for "which locations does this statement write". Its defect
# class -- a statement shape the analyzer failed to enumerate, so a pass missed
# a write -- is driven directly here, because `bin/cse` cannot isolate it: a
# missed write is only observable through the pass that trusted it. The shape
# table pins the historical escapes (for-init reference, declaration
# initializer, element store) as a class, not one fixture at a time.
echo "=== write-set shapes ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_write_set.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_write_set"
"$WORK/v_write_set" | grep -q "ALL WRITE-SET CHECKS PASSED"
echo "ok    write-set shapes"

# The purity questions (analysis/purity.h) are one authority now; a pass that
# asks the shallow question where the deep one is needed is exactly the class
# that duplicated `2.0 * (f() + 1.0)`. Pin the shallow/deep distinction on nodes
# built through the real factories.
echo "=== purity questions ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_purity.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_purity"
"$WORK/v_purity" | grep -q "ALL PURITY CHECKS PASSED"
echo "ok    purity questions"

# The lexer is the single authority for "is this a preprocessing directive", and
# the region extractor's flag follows from its token stream. Pinned directly, in
# both directions: a directive that follows a comment on the same line must be
# recognized (it IS one), and a '#' inside a comment or string must not be.
echo "=== preprocessor-directive extraction ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" \
  "$VERIFY/verify_pp_extraction.cpp" "$ROOT/bin/libcse.a" -o "$WORK/v_ppx"
"$WORK/v_ppx" | grep -q "ALL PP-EXTRACTION CHECKS PASSED"
echo "ok    directive-vs-comment decision"

# DAGNode::sameContentAs() and computeHash() now expand one field table
# (CSE_DAGNODE_IDENTITY_FIELDS), so they cannot compare/hash different sets.
# This pins the table from the outside: every field that changes what a node
# denotes must make two nodes differ *and* hash apart, so a field that is added
# to DAGNode but not to the table is caught rather than silently dropped from
# identity.
echo "=== node identity table ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_node_identity.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_node_id"
"$WORK/v_node_id" | grep -q "ALL NODE-IDENTITY CHECKS PASSED"
echo "ok    node identity table"

# ---------------------------------------------------------------------------
# 2c. round-trip fidelity oracle (frontend <-> backend text boundary)
#     For every //@cse region: parse -> build IR -> codegen -> parse -> build
#     IR, then compare the two IR trees structurally. Catches printer defects
#     (dropped parens, mis-printed operators) that leave the IR correct and so
#     are invisible to every value-level verifier. Skips regions whose IR this
#     oracle does not model (structs/namespaces/declarations).
# ---------------------------------------------------------------------------
echo "=== round-trip fidelity ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_roundtrip.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_roundtrip"
rt_out="$("$WORK/v_roundtrip" "$FIXTURES"/*.cpp || true)"
echo "$rt_out" | tail -1
if echo "$rt_out" | tail -1 | grep -q ", 0 failed)"; then
  echo "ok    round-trip (emitted text re-parses to the same IR)"
else
  echo "FAIL  round-trip: emitted text does not re-parse to the same IR" >&2
  echo "$rt_out" | grep -A30 '^FAIL' | head -80 >&2 || true
  exit 1
fi

# ---------------------------------------------------------------------------
# 2c'. pass idempotence (Phase D3): running any single pass a second time must
#      be a no-op. Uses the structural IR fingerprint, so a pass that merely
#      reorders operands (reassociation) is not mistaken for a change. Surfaces
#      passes whose result depends on how many times they run -- e.g. the
#      algebraic pass once rewrote `x * 2.0` to `2.0 * x` and only strength-
#      reduced it to `x + x` on a second application.
# ---------------------------------------------------------------------------
echo "=== pass idempotence ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" \
  "$VERIFY/verify_ir_idempotence.cpp" "$ROOT/bin/libcse.a" -o "$WORK/v_idem"
idem_out="$("$WORK/v_idem" "$FIXTURES"/*.cpp || true)"
echo "$idem_out" | tail -1
if echo "$idem_out" | tail -1 | grep -q ", 0 failed)"; then
  echo "ok    pass idempotence (each pass is a projection)"
else
  echo "FAIL  pass idempotence: a pass changed its own output" >&2
  echo "$idem_out" | grep -A30 '^FAIL' | head -80 >&2 || true
  exit 1
fi

# ---------------------------------------------------------------------------
# 2c''. effect preservation (Phase D2): no pass may drop an observable effect
#       (an impure call, an increment/decrement) or, unless it declares
#       mayDuplicateEffects, add one. Catches `2*f() -> f()+f()`, DCE deleting
#       `unused = y++`, and an algebraic identity discarding an effectful
#       operand. Unit checks prove the oracle fires; the sweep runs it over
#       every fixture region.
# ---------------------------------------------------------------------------
echo "=== effect preservation ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" \
  "$VERIFY/verify_effect_preservation.cpp" "$ROOT/bin/libcse.a" -o "$WORK/v_eff"
eff_out="$("$WORK/v_eff" "$FIXTURES"/*.cpp || true)"
echo "$eff_out" | tail -1
if echo "$eff_out" | tail -1 | grep -q ", 0 failed)" &&
   ! echo "$eff_out" | grep -q "FAIL"; then
  echo "ok    effect preservation (no pass drops or duplicates an effect)"
else
  echo "FAIL  effect preservation: a pass changed the effect multiset" >&2
  echo "$eff_out" | grep -B1 -A10 'FAIL' | head -60 >&2 || true
  exit 1
fi

# ---------------------------------------------------------------------------
# 2c'''. pass contracts (Phase D5): every pass wired into the pipeline must be
#        able to fire (a pass that changes nothing across the whole suite is
#        dead or gated shut -- how `-s -r` silently dropped recombination), and
#        only LoopUnroll may forgo effect preservation.
# ---------------------------------------------------------------------------
echo "=== pass contracts ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" \
  "$VERIFY/verify_pass_contracts.cpp" "$ROOT/bin/libcse.a" -o "$WORK/v_pc"
pc_out="$("$WORK/v_pc" "$FIXTURES"/*.cpp || true)"
echo "$pc_out"
if echo "$pc_out" | tail -1 | grep -q "OK"; then
  echo "ok    every pass fires; effect contracts as declared"
else
  echo "FAIL  a pass contract was broken" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 2c''''. IR invariant gate (Phase D6): run the whole pipeline over every
#         fixture with the invariant checks on (IRModule::verify() plus effect
#         preservation), and drive the same through the driver's --verify flag.
#         `make sanitize` runs this stage under ASan/UBSan. Exit 4 means a pass
#         broke an invariant; no fixture may produce it.
# ---------------------------------------------------------------------------
echo "=== IR invariant gate ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" \
  "$VERIFY/verify_ir_invariants.cpp" "$ROOT/bin/libcse.a" -o "$WORK/v_ii"
ii_out="$("$WORK/v_ii" "$FIXTURES"/*.cpp || true)"
echo "$ii_out" | tail -1
if ! echo "$ii_out" | tail -1 | grep -q "IR-INVARIANTS OK"; then
  echo "FAIL  an IR invariant was violated" >&2
  echo "$ii_out" | grep -A20 '^FAIL' | head -60 >&2 || true
  exit 1
fi
mkdir -p "$WORK/verify"
vfail=0
for f in "$FIXTURES"/*.cpp; do
  cp "$f" "$WORK/verify/$(basename "$f")"
  set +e
  "$CSE" --verify "$WORK/verify/$(basename "$f")" >/dev/null 2>&1
  rc=$?
  set -e
  if [[ "$rc" -eq 4 ]]; then
    echo "FAIL  --verify reported an invariant violation for $(basename "$f")" >&2
    vfail=1
  fi
done
(( vfail == 0 )) || exit 1
echo "ok    IR invariants hold; bin/cse --verify never exits 4 on a fixture"

# ---------------------------------------------------------------------------
# 2d. lexer token census: no byte is silently dropped, multi-char operators lex
#     as one token. Phase B3 completed the operator set (`|`, `^`, `~`, `%=`,
#     `&=`, `|=`, `^=`), so this is now a hard gate.
# ---------------------------------------------------------------------------
echo "=== lexer token census ==="
"$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 -I"$ROOT/src" "$VERIFY/verify_lexer.cpp" \
  "$ROOT/bin/libcse.a" -o "$WORK/v_lexer"
if "$WORK/v_lexer" >"$WORK/lexer.out" 2>&1; then
  echo "ok    lexer census"
else
  echo "FAIL  lexer census: a byte was silently dropped or an operator mis-lexed" >&2
  sed 's/^/      /' "$WORK/lexer.out" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 2e. region coverage: a marked region that parsed to nothing must be reported
#     as skipped (never silently passed through as if optimized), and a
#     fully-supported fixture must report zero skips.
# ---------------------------------------------------------------------------
echo "=== region coverage reporting ==="
cov_fail=0
printf '//@cse\ndouble g = 5.0;\n' > "$WORK/decl_only.cpp"
"$CSE" "$WORK/decl_only.cpp" > /dev/null 2> "$WORK/decl_only.err" || true
if ! grep -q "region skipped" "$WORK/decl_only.err"; then
  echo "FAIL  a declaration-only region was not reported as skipped" >&2
  cov_fail=1
fi
if ! grep -q "1 skipped-unsupported" "$WORK/decl_only.err"; then
  echo "FAIL  coverage did not count the skipped-unsupported region" >&2
  sed 's/^/      /' "$WORK/decl_only.err" >&2
  cov_fail=1
fi
cp "$FIXTURES/basic_cse.cpp" "$WORK/basic_cse.cpp"
"$CSE" "$WORK/basic_cse.cpp" > /dev/null 2> "$WORK/basic_cse.err"
if ! grep -q "0 skipped-unsupported" "$WORK/basic_cse.err"; then
  echo "FAIL  a fully-supported fixture reported a skipped region" >&2
  sed 's/^/      /' "$WORK/basic_cse.err" >&2
  cov_fail=1
fi
(( cov_fail == 0 )) || exit 1
echo "ok    marked-but-empty regions are reported; supported fixtures report 0 skips"

# ---------------------------------------------------------------------------
# 2i. outcome contract (Phase B4): every region lands in exactly one bucket,
#     a frontend rejection carries a machine-readable code, and an unsupported
#     region is named rather than silently dropped.
# ---------------------------------------------------------------------------
echo "=== outcome contract ==="
oc_fail=0
grep -q "\[CSE-LEX-UNEXPECTED-BYTE\]" "$WORK/containment.err" ||
  { echo "FAIL  a lex rejection did not carry its machine-readable code" >&2; oc_fail=1; }
grep -q "CSE-REGION-UNSUPPORTED" "$WORK/decl_only.err" ||
  { echo "FAIL  an unsupported region was not named" >&2; oc_fail=1; }
grep -q "skipped-unsupported, 0 aborted-internal)" "$WORK/decl_only.err" ||
  { echo "FAIL  coverage line does not report the full bucket set" >&2; oc_fail=1; }
(( oc_fail == 0 )) || exit 1
echo "ok    diagnostics carry codes; every region lands in one bucket"

# ---------------------------------------------------------------------------
# 2g. marker robustness (Phase B2). The driver used to rewrite the source
#     before region extraction: it searched for the first `//` on each line and
#     truncated everything after `@cse`, so a marker with trailing text lost the
#     text and a string literal containing the marker spelling was corrupted
#     into an unterminated one. Marker recognition now lives only in the region
#     extractor. Assert the marker line, a string and a block comment all come
#     through byte-identical, and that a spaced `// @cse` is still recognized.
# ---------------------------------------------------------------------------
echo "=== marker robustness ==="
cat > "$WORK/marker.cpp" <<'EOF'
// @cse trailing note kept verbatim
double f(double x) { return x + x; }
/* a block comment mentioning //@cse must survive */
const char* s = "// @cse inside a string must survive";
EOF
"$CSE" "$WORK/marker.cpp" >/dev/null 2> "$WORK/marker.err" || true
mk_fail=0
grep -q "1 optimized" "$WORK/marker.err" ||
  { echo "FAIL  spaced '// @cse' marker was not recognized" >&2; mk_fail=1; }
grep -qxF '// @cse trailing note kept verbatim' "$WORK/marker.cpp.cse" ||
  { echo "FAIL  marker-line trailing text was dropped" >&2; mk_fail=1; }
grep -qxF '/* a block comment mentioning //@cse must survive */' "$WORK/marker.cpp.cse" ||
  { echo "FAIL  block-comment line changed" >&2; mk_fail=1; }
grep -qxF 'const char* s = "// @cse inside a string must survive";' "$WORK/marker.cpp.cse" ||
  { echo "FAIL  string-literal line changed" >&2; mk_fail=1; }
(( mk_fail == 0 )) || exit 1
echo "ok    spaced markers recognized; marker line, string and comment bytes preserved"

# ---------------------------------------------------------------------------
# 2h. token-based region boundaries (Phase B1). Region extent is decided from
#     the lexer's token stream, so a brace inside a character literal, string
#     or comment cannot move the boundary. If the `{` in `'{'` were counted, the
#     first region would run past its own closing brace and swallow the second;
#     the second must still be found and optimized.
# ---------------------------------------------------------------------------
echo "=== token-based region boundaries ==="
cat > "$WORK/boundary.cpp" <<'EOF'
//@cse
double a1(double x) { char c = '{'; return x; }
//@cse
double a2(double x, double y) { return (x + y) * (x + y); }
EOF
"$CSE" "$WORK/boundary.cpp" >/dev/null 2> "$WORK/boundary.err" || true
if ! grep -q "2 regions (1 optimized, 1 skipped-unreadable" "$WORK/boundary.err"; then
  echo "FAIL  a char-literal brace desynced the region boundary" >&2
  sed 's/^/      /' "$WORK/boundary.err" >&2
  exit 1
fi
echo "ok    brace inside a char literal does not extend the region"

# ---------------------------------------------------------------------------
# 2f. twin differential execution (-s). For a fixture following the sf_/ref_
#     convention, generate a main that calls each pair on a seeded random
#     stream plus edge values and compares. Runs on the conservative profile,
#     where behavior must be exact. This is the only oracle that sees the
#     *semantics* of the optimized code for arbitrary inputs.
# ---------------------------------------------------------------------------
echo "=== twin differential (-s) ==="
# twin_run <fixture> [min-pairs]
#   Optimize the fixture under -s, generate a differential driver from its
#   `sf_`/`ref_` twins, and run it. Fails unless it prints
#   "TWIN-DIFF PASSED (N pairs)" with N >= min-pairs: a run with no comparable
#   pair (a renamed/deleted `ref_` twin) used to print SKIPPED and exit 0, so the
#   fixture's only value oracle vanished while the suite stayed green.
twin_run() {
  local fx="$1" minpairs="${2:-1}" base
  base="$(basename "$fx" .cpp)"
  mkdir -p "$WORK/twin"
  cp "$fx" "$WORK/twin/$base.cpp"
  "$CSE" -s "$WORK/twin/$base.cpp" >/dev/null 2>&1 || true
  python3 "$VERIFY/gen_twin_main.py" "$WORK/twin/$base.cpp.cse" \
    "$WORK/twin/${base}_main.cpp" 2>/dev/null
  "$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 "$WORK/twin/${base}_main.cpp" \
    -o "$WORK/twin/$base" || return 2
  local out
  out="$("$WORK/twin/$base")" || { printf '%s\n' "$out"; return 1; }
  local n
  n="$(sed -n 's/.*TWIN-DIFF PASSED (\([0-9]*\) pairs).*/\1/p' <<<"$out")"
  if [[ -z "$n" || "$n" -lt "$minpairs" ]]; then
    printf '%s\n' "$out"
    printf 'twin_run: want >= %s comparable pairs\n' "$minpairs"
    return 1
  fi
  printf '%s\n' "$out"
}

if out="$(twin_run "$FIXTURES/semantics_fixes.cpp" 22)"; then
  echo "ok    semantics_fixes twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  semantics_fixes twin diff" >&2
  echo "$out" >&2
  exit 1
fi

# Reference-alias family (Phase C1): a write through a reference local must
# invalidate its referent. The twin pairs cover a direct binding, a chain
# (`s -> r -> x`) and a compound store. Closed by the AliasModel closure.
if out="$(twin_run "$VERIFY/twins/ref_write.cpp" 5)"; then
  echo "ok    ref_write twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  ref_write twin diff (a reference-local write did not invalidate its referent)" >&2
  echo "$out" >&2
  exit 1
fi

# A `break;` / `continue;` inside a braced block must survive. CleanupPass used
# to erase it as a dead variable reference once it learned to recurse into
# nested blocks; the value oracle catches the control-flow change.
if out="$(twin_run "$FIXTURES/loop_control.cpp" 3)"; then
  echo "ok    loop_control twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  loop_control twin diff (break/continue erased)" >&2
  echo "$out" >&2
  exit 1
fi

# A loop whose counter escapes (`return s + i;` after `for (int i = ...)`) is not
# valid C++ input, so it cannot be a compiled fixture. The unroller must still
# leave it verbatim: splicing the body out would turn one compile error into a
# different one, or silently rebind to an outer `i`. The shape check is above;
# the compiled fixture covers the controls (contained counters still unroll).
if out="$(twin_run "$FIXTURES/loop_escape.cpp" 3)"; then
  echo "ok    loop_escape twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  loop_escape twin diff (a contained counter stopped unrolling)" >&2
  echo "$out" >&2
  exit 1
fi

# DCE must reach into nested scopes. It only ever pruned the top-level block, so
# a dead zero-init local inside an `if` or a `for` body survived every round --
# while CleanupPass, which does recurse, removed its neighbours.
if out="$(twin_run "$FIXTURES/dce_nested.cpp" 3)"; then
  echo "ok    dce_nested twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  dce_nested twin diff (nested dead code changed a value)" >&2
  echo "$out" >&2
  exit 1
fi

# A constant's int/float kind must survive folding and dedup: `4.0/3` and `4/3`
# are different values, and `x/2.0 + y/2` must keep `y/2` an integer division.
if out="$(twin_run "$FIXTURES/const_typing.cpp" 5)"; then
  echo "ok    const_typing twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  const_typing twin diff (a constant changed its int/float kind)" >&2
  echo "$out" >&2
  exit 1
fi

# The write-set defect class, end to end: a reference declared in a for-init, a
# reference chain, and a value reused across a write to one of its operands.
# `verify_write_set.cpp` pins the analysis shape; these twins pin that the
# optimized code still computes the same value for the same shapes.
if out="$(twin_run "$FIXTURES/write_set_shapes.cpp" 4)"; then
  echo "ok    write_set_shapes twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  write_set_shapes twin diff (a write was missed and a stale value forwarded)" >&2
  echo "$out" >&2
  exit 1
fi

# The reference read / rebind defect class: a referent inlined into its own
# binding (ill-formed), and an expression read *through* a reference reused
# across a write to the referent (wrong value). The first fails to compile, so
# this twin run must both build and match.
# The for-update effect fixture now carries runnable sf_/ref_ twins. Its first
# draft had no terminating loop (the counter was never advanced), which is why
# the fixture existed but its value oracle never ran -- the effect-preserving
# sweep covered it, but only structurally.
if out="$(twin_run "$FIXTURES/effect_for_update.cpp" 1)"; then
  echo "ok    effect_for_update twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  effect_for_update twin diff (a non-counter for-update increment changed the value)" >&2
  echo "$out" >&2
  exit 1
fi

if out="$(twin_run "$FIXTURES/ref_rebind.cpp" 3)"; then
  echo "ok    ref_rebind twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  ref_rebind twin diff (a reference read/rebind was miscompiled)" >&2
  echo "$out" >&2
  exit 1
fi

# A hoisting rewrite may not make a guarded computation run unconditionally when
# it can trap. `k ? 1/k : 0` written twice had its division extracted to the top
# and trapped at k = 0 (SIGFPE); `&&` had the same shape. The driver's edge-value
# stream includes k = 0, so the crash is a hard failure here rather than a value
# mismatch. `spec_pure_arm` is the control: an ordinary product inside the same
# conditional may still be hoisted.
if out="$(twin_run "$FIXTURES/speculation.cpp" 4)"; then
  echo "ok    speculation twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  speculation twin diff (a guarded computation was hoisted)" >&2
  echo "$out" >&2
  exit 1
fi

# A pass that invents a name must not collide with a user identifier. Variables
# are interned by name, so `_cse_0_4` (CSE's temporary) and a user variable of
# the same spelling are one node; likewise `t__u0` (unroller) and `x__s1`
# (builder). The twin differential catches the value change, and the generated
# output must still compile (the parameter-shadow case is a compile error).
if out="$(twin_run "$FIXTURES/fresh_names.cpp" 3)"; then
  echo "ok    fresh_names twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  fresh_names twin diff (a generated name collided with a user identifier)" >&2
  echo "$out" >&2
  exit 1
fi

# A declaration inside a region may not be confused with a name the region
# reads but does not declare. Variables are interned by name, so `double s = t;`
# (a global) and a later `double t = a;` used to be one node, and value
# propagation inlined the local into the global read.
if out="$(twin_run "$FIXTURES/shadow_global.cpp" 3)"; then
  echo "ok    shadow_global twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  shadow_global twin diff (a local shadowed an external name)" >&2
  echo "$out" >&2
  exit 1
fi

# The flagship fixtures (basic_cse, features, namespace_case, cost_nested) were
# pinned only by a FLOP count and a structural grep, neither of which observes
# the value the optimized code computes. This fixture reproduces their scalar
# shapes so the twin driver can compare `sf_` against `ref_` on real inputs.
if out="$(twin_run "$VERIFY/twins/flagship.cpp" 5)"; then
  echo "ok    flagship twin diff ($(echo "$out" | tail -1))"
else
  echo "FAIL  flagship twin diff (a flagship shape changed its value)" >&2
  echo "$out" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 2j. generative differential fuzz (-s). The curated fixtures above each pin a
#     shape a human thought of. This draws programs from a grammar of the same
#     write-set / reference shapes and runs the twin differential on each, so a
#     shape nobody wrote a fixture for -- an analyzer that fails to enumerate
#     it, or a pass that reuses a value across a write it did not see -- surfaces
#     as a compile failure or a value mismatch on its own.
#
#     Seed -> program is deterministic (random.Random(seed)); the twin driver
#     uses a fixed input stream, so a failure is reproducible from the seed.
#     Runs under -s, where the optimized twin must reproduce the untouched one
#     bit for bit.
# ---------------------------------------------------------------------------
echo "=== generative differential fuzz (-s) ==="
FUZZ_SEEDS="${CSE_FUZZ_SEEDS:-32}"
FUZZ_FUNCS="${CSE_FUZZ_FUNCS:-6}"
fuzz_fail=0
fuzz_changed=0
for ((seed = 1; seed <= FUZZ_SEEDS; seed++)); do
  gen="$WORK/fuzz_$seed.cpp"
  python3 "$VERIFY/gen_shapes.py" "$seed" "$FUZZ_FUNCS" "$gen"
  # A region the frontend cannot read is passed through unchanged; the compile
  # and differential below then simply confirm the passthrough is still legal.
  "$CSE" -s "$gen" >/dev/null 2>"$WORK/fuzz_$seed.err" || true
  if ! "$CXX" $EXTRA_CXXFLAGS -std=c++17 -fsyntax-only "$gen.cse" \
       2>"$WORK/fuzz_$seed.cerr"; then
    echo "FAIL  fuzz seed $seed: the optimized output does not compile" >&2
    sed 's/^/      /' "$WORK/fuzz_$seed.cerr" | head -5 >&2
    fuzz_fail=1
    continue
  fi
  python3 "$VERIFY/gen_twin_main.py" "$gen.cse" "$WORK/fuzz_${seed}_main.cpp" \
    2>/dev/null
  if ! "$CXX" $EXTRA_CXXFLAGS -std=c++17 -O2 "$WORK/fuzz_${seed}_main.cpp" \
       -o "$WORK/fuzz_$seed" 2>/dev/null; then
    echo "FAIL  fuzz seed $seed: the differential driver did not build" >&2
    fuzz_fail=1
    continue
  fi
  if ! out="$("$WORK/fuzz_$seed")"; then
    echo "FAIL  fuzz seed $seed: the twin differential mismatched" >&2
    printf '%s\n' "$out" | head -3 >&2
    fuzz_fail=1
    continue
  fi
  # Non-vacuity bookkeeping: did the optimizer change this program at all?
  if ! diff -q <(grep -v '^//' "$gen") <(grep -v '^//' "$gen.cse") >/dev/null; then
    fuzz_changed=$((fuzz_changed + 1))
  fi
done
(( fuzz_fail == 0 )) || { echo "FAIL  generative fuzz found a miscompile" >&2; exit 1; }
# If the optimizer never changed anything, the differential would pass
# trivially. Require that most seeds were actually rewritten.
if (( fuzz_changed * 2 < FUZZ_SEEDS )); then
  echo "FAIL  fuzz is vacuous: only $fuzz_changed/$FUZZ_SEEDS seeds were optimized" >&2
  exit 1
fi
echo "ok    $FUZZ_SEEDS seeds optimized and value-exact ($fuzz_changed changed)"

# The grammar must actually produce the shape families it claims, or the extra
# seeds are vacuous: a family that regenerates nothing is indistinguishable from
# one the optimizer handles. Each family therefore asserts a probe seed contains
# at least one of its statements.
echo "=== fuzzer grammar coverage ==="
python3 "$VERIFY/gen_shapes.py" 1 8 "$WORK/grammar_probe.cpp"
gcov_fail=0
if ! grep -qE '^  double arr[0-9]+\[[0-9]+\];' "$WORK/grammar_probe.cpp"; then
  echo "FAIL  the fuzzer grammar no longer generates local array declarations" >&2
  gcov_fail=1
fi
if ! grep -qE '^  arr[0-9]+\[[0-9]+\] = ' "$WORK/grammar_probe.cpp"; then
  echo "FAIL  the fuzzer grammar no longer generates element stores" >&2
  gcov_fail=1
fi
if ! grep -qE '^  for \(; i[0-9]+ < [0-9]+; \+\+' "$WORK/grammar_probe.cpp"; then
  echo "FAIL  the fuzzer grammar no longer generates non-counter for-update increments" >&2
  gcov_fail=1
fi
(( gcov_fail == 0 )) || exit 1
echo "ok    arrays, element stores and non-counter updates are in the grammar"

# ---------------------------------------------------------------------------
# 2k. the generated output must still compile.
#
#     Every other oracle compares values or IR shapes; none of them notices a
#     region the optimizer turned into *invalid C++*. Two such defects have been
#     found by hand (`return inf;` from folding an overflow, and a chain that
#     referenced a declaration the unroller had erased), so this is a class-level
#     gate now: optimize every fixture and syntax-check the result.
#
#     Two fixtures are exempt because their *input* does not compile either --
#     they are parse/cost fixtures that reference types the file never defines
#     (`features` does `px->x` on a `double*`, `cost_nested` multiplies a
#     locally-defined `Vector` by a scalar with no such operator). They are
#     listed explicitly rather than silently skipped: a new fixture that fails to
#     compile is a failure, not a skip.
# ---------------------------------------------------------------------------
echo "=== generated output compiles ==="
SYNTAX_EXEMPT="features cost_nested"
syntax_fail=0
for f in "$FIXTURES"/*.cpp; do
  base=$(basename "$f" .cpp)
  skip=0
  for ex in $SYNTAX_EXEMPT; do [[ "$base" == "$ex" ]] && skip=1; done
  (( skip == 1 )) && continue
  cp "$f" "$WORK/syn_$base.cpp"
  "$CSE" "$WORK/syn_$base.cpp" >/dev/null 2>&1 || true
  if ! "$CXX" -x c++ -std=c++17 -fsyntax-only "$WORK/syn_$base.cpp.cse" \
       2>"$WORK/syn_$base.err"; then
    echo "FAIL  $base: the optimized output is not valid C++" >&2
    sed 's/^/      /' "$WORK/syn_$base.err" | head -5 >&2
    syntax_fail=1
  fi
done
(( syntax_fail == 0 )) || exit 1
echo "ok    every optimizable fixture's output still compiles"

# The exhaustive-switch convention (explicitly list every StmtIRKind so a new
# one is a compile error, not a silent skip) has no compiler guard: -Wall
# -Wextra does not flag a `default:` that swallows a new enumerator, and
# -Wswitch-enum is too noisy to enable globally. This shape check pins the
# convention instead: a statement-kind switch must be exhaustive or carry a
# `// partial:` marker stating that handling only some kinds is deliberate.
# Node-kind switches are out of scope -- many of them classify only a subset
# of node kinds by design (see check_stmt_switches.py's docstring).
if out="$(python3 "$VERIFY/check_stmt_switches.py" "$ROOT")"; then
  echo "$out" | sed 's/^/      /'
else
  echo "FAIL  statement-kind switch shape check" >&2
  echo "$out" >&2
  exit 1
fi
echo "ok    statement-kind switches stay exhaustive (or marked partial:)"

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

# Self-assignment regression guard: CleanupPass removes `x = x` / `a[i] = a[i]`
# (including the vector-lowering artifacts, `u_value[i] = u_value[i];`), which is
# value-neutral and therefore invisible to every numeric verifier. Assert the
# emitted headers contain none.
echo "=== self-assignment guard (csegen) ==="
if python3 "$VERIFY/check_self_assign.py" "$WORK"/equilibrium.ur.h \
     "$WORK"/force.ur.h "$WORK"/moment.ur.h >/dev/null; then
  echo "ok    generated headers have no self-assignment"
else
  echo "FAIL  a generated header contains a self-assignment" >&2
  python3 "$VERIFY/check_self_assign.py" "$WORK"/equilibrium.ur.h \
    "$WORK"/force.ur.h "$WORK"/moment.ur.h || true
  exit 1
fi

# csegen has no passthrough mode: it emits a specialization or nothing. A
# directive inside a marked struct cannot be regenerated either (there is no AST
# node for it), so it must fail loudly instead of emitting the `.ur.h` with the
# guard deleted and `#if 0`'s body live -- a wrong-code result compiled into the
# real FreeLB build, which is the worst place for this to surface.
echo "=== csegen rejects directive regions ==="
mkdir -p "$WORK/dirgen"
cat > "$WORK/dirgen/moment.h" <<'EOF'
// @cse
template <typename CELLTYPE>
struct GuardedImpl {
  using T = typename CELLTYPE::FloatType;
  __any__ static inline void apply(CELLTYPE& cell, T& v) {
#if 0
    v = 999.0;
#endif
    v = cell[0] + cell[0];
  }
};
EOF
dirgen_fail=0
set +e
"$CSEGEN" "$WORK/dirgen/moment.h" "$WORK/dirgen/moment.ur.h" \
  >/dev/null 2> "$WORK/dirgen.err"
dirgen_rc=$?
set -e
if [[ "$dirgen_rc" -eq 0 ]]; then
  echo "FAIL  csegen accepted a region with a directive (exit 0)" >&2
  dirgen_fail=1
elif [[ -f "$WORK/dirgen/moment.ur.h" ]]; then
  echo "FAIL  csegen wrote an output file for a directive region" >&2
  dirgen_fail=1
fi
if ! grep -q "preprocessor directive in a //@cse region" "$WORK/dirgen.err"; then
  echo "FAIL  csegen did not name the reason it refused the header" >&2
  sed 's/^/      /' "$WORK/dirgen.err" >&2
  dirgen_fail=1
fi
(( dirgen_fail == 0 )) || exit 1
echo "ok    csegen refuses to specialize a directive region"

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
    # The pinned reference is the hand-written src/ur_fallback/lbm/*.ur.h, which
    # FreeLB snapshots to tools/cse/reference/ (tools/cse/Makefile `gen-refs`).
    # Probe the canonical location first, then the legacy src/lbm/ path, so a
    # moved reference does not silently turn this verifier into a no-op.
    ref=""
    for cand in "$FREELB/tools/cse/reference/$v.ur.h" \
                "$FREELB/src/lbm/$v.ur.h" \
                "$FREELB/src/ur_fallback/lbm/$v.ur.h"; do
      if [[ -f "$cand" ]]; then ref="$cand"; break; fi
    done
    if [[ -z "$ref" ]]; then
      echo "skip  $v (no pinned reference under tools/cse/reference/ or src/lbm/)"
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
