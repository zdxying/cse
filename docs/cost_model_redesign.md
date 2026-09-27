# CSE Cost Model — Redesign

Status: implemented (this document describes the target model and the
decisions taken while replacing the original ad-hoc FLOP counter).

## Motivation

The original `analyzeCost` (see `src/analysis/cost_model.{h,cpp}`) counted a
single `BinaryOp` as one FLOP and ignored everything else. For FreeLB code
that is a poor approximation; several constructs were mis-evaluated:

1. **Vector arithmetic was counted as scalar.**
   `createFreeLBConfig` populated `vectorDim`/`isVectorType` but never enabled
   `lowerVectors`, and the cost model ignored vector-ness entirely. A
   `Vector<T, d>` op (`u_value += latset::c<LatSet>(i) * cell[i]`,
   `u_value /= rho_value`, `f_alpha * T{0.5}`) compiled to `d` scalar ops but
   scored 1.
2. **Opaque pure helpers scored 0.** `getnorm2`, `getnorm`, `dot`, `getsum`,
   `latset::c/w`, `clear`, field getters are `Call` nodes. The architecture doc
   already admitted `u.getnorm2()` hid 5 FLOPs.
3. **Loop trip counting was broken by the `latticeQ` guard.** `loopTripCount`
   returned `0` whenever a counted loop's bound `!= q` and a lattice was set,
   collapsing the nested `alpha/beta < LatSet::d` moment loops
   (`src/lbm/moment.h`) to one iteration (undercount ~3–6x). Non-resolved
   bounds were silently counted once.
4. **Shared subexpressions were over-counted.** `visited` was cleared per
   statement, so a DAG node reused by several statements was charged once per
   statement, contradicting the CSE-materialized temporaries in the output.
5. **No coverage signal.** Unknown loops and unmodeled calls were silently
   folded into the numbers with no way to tell when a figure was unreliable.
6. **Driver double-work.** `plugins/freelb/cse_main.cpp` re-ran
   `optimizeRegion` for every region just to collect per-function costs even
   though `functionCosts` was already available.

## Decisions (agreed semantics)

| Topic | Decision |
|-------|----------|
| FLOP unit | **Scalar FLOPs, vector-weighted**: a vector op costs its lane count; a vector·vector product is a dot = `2d-1`. |
| Helper/intrinsic calls | **Known-weight hook**: the FreeLB plugin supplies per-callee costs; unknown calls are flagged unmodeled. |
| Shared subexpressions | **Unique DAG nodes**: each distinct computation is charged once, matching CSE temporaries. |
| Memory | **Separate `memOps`** metric: unique loads (`array`/`member`/`arrow` accesses) plus one per complex-lvalue store. |

Loop multiplicity is *dynamic*: a unique computation inside a loop is charged
once per execution, i.e. `cost × (number of enclosing loop iterations)`. This
reconciles the unique-node rule (a CSE temp computed once outside a loop is
charged once) with runtime work (a body computation is charged per iteration).

## Model

`CostResult` (`src/analysis/cost_model.h`):

```cpp
struct CostResult {
  long long flops = 0;          // scalar, vector-weighted
  long long vectorOps = 0;      // ops with at least one vector operand
  long long memOps = 0;         // unique loads + stores
  long long totalNodes = 0;     // unique DAG nodes
  long long stmts = 0;          // executed statements (loop-scaled)
  long long vars = 0;           // executed declarations (loop-scaled)
  long long unknownLoops = 0;   // trip count could not be resolved
  long long unmodeledCalls = 0; // no cost entry for the callee
};
```

### Counting rules

* Traverse statements in program order with a **single global `visited` set**
  over `DAGNode*`. The first time a node is seen it is charged
  `perExecCost(node) × execCount`, where `execCount` is the number of
  executions of the current loop context.
* `perExecCost`:
  * `BinaryOp` with `+ - * /`: lanes come from operand lanes; the FreeLB
    `binaryOpCost` hook returns `{flops, resultLanes}`. Default (no hook):
    1 FLOP, scalar result.
  * `UnaryOp +/-`: lane-count FLOPs (vector) or 1.
  * `ArrayAccess`/`MemberAccess`/`ArrowAccess`: `memOps += 1` (unique loads).
  * `Assign` with a complex lvalue (`targetExpr`): `memOps += 1` per execution
    (stores); its base/index operands are traversed as usual.
  * `Call`: `callCost(callee, argLanes)`; `>= 0` adds FLOPs, `< 0` marks
    unmodeled. No `callCost` hook (generic mode) charges 0 without flagging.
  * `Ternary`/`Cast`: recurse into operands, no direct cost.
* `if/else` sums both branches (worst case), unchanged.

### Lane inference

`vecDim` metadata is attached to `DAGNode` by `IRBuilder` (from declared
parameter/local types via `CSEConfig::vectorLanes`). Compound expression lanes
are computed on the fly by the cost model:

* `Variable` → stored `vecDim`.
* `Call` → `callResultLanes(callee)` (FreeLB parses `D<n>Q<q>` and falls back
  to the configured lattice dimension).
* `BinaryOp` → hook; default scalar.
* `UnaryOp`/`Cast`/`Ternary` → propagate.

`vecDim` is **not** part of the structural hash (`recomputeHash`), so CSE
deduplication is unaffected.

### Loop iteration counting

A stack of canonical `LoopDesc {var, start, bound, step, inclusive}` is
maintained during traversal. Both ascending (`<`/`<=`, `++`/`+=k`) and
descending (`>`/`>=`, `--`/`-=k`) counted loops are recognized; the step sign
must agree with the comparison direction. `execCount` computes the number of
executions of the current context:

* **Rectangular nests** take a fast path: when no loop extent references an
  enclosing loop variable, the count is the product of the per-loop trip counts.
* **Dependent nests** (e.g. the triangular `for (b = a; b < N; ++b)` inside
  `a`) are enumerated exactly:

  ```
  execCount(stack, i):
    if i == len(stack): return 1
    for v in [start_i, bound_i) step step_i:
        bind(var_i, v); count += execCount(stack, i+1)
    return count
  ```

  Enumeration is bounded by `kMaxEnum` (1e6); a larger dependent nest falls back
  to the max-product estimate, which may **over-count**.
* Loops whose bounds/steps cannot be resolved, or whose sign/direction
  disagree, are charged once and increment `unknownLoops`.

This is cheap for the FreeLB dim (`d<=3`) and lattice (`q<=27`) loops.

### Hooks (`src/frontend/cse_config.h`)

```cpp
std::function<int(const std::string& type)> vectorLanes;               // declared type -> lanes
std::function<int(const std::string& callee)> callResultLanes;         // callee -> result lanes
std::function<long long(const std::string& callee,
                        const std::vector<int>& argLanes)> callCost;   // -1 = unmodeled
std::function<int(char op, int aLanes, int bLanes, long long& flops)> binaryOpCost;
```

`plugins/freelb/config.h` wires these from the active `LatticeConfig`:

* `vectorLanes`: parse `Vector<..., N>` (numeric) or use the lattice dimension
  for `Vector<T, LatSet::d>`.
* `callResultLanes`: parse `D<n>Q<q>` from the callee, else lattice dimension.
* `callCost`: `getnorm2 -> 2d-1`, `getnorm -> 2d`, `getsum -> max(0, d-1)`,
  `dot -> 2d-1`, `latset::*`/`lattice::*`/`clear`/`get*` -> 0, builtin math -> 1,
  otherwise `-1`. Vector-valued `getnorm*`/`dot`/`getsum` need a known lane
  count (`d > 0`, from a lattice dimension or a numeric `Vector<..., N>`); with
  no lane information they are treated as unmodeled (`-1`).
* `binaryOpCost`: `*` = dot when both operands are vectors (`2d-1`, scalar
  result) else scaling (lane count); `+ - /` = componentwise (max lanes).

Generic `createFreeLBConfig()` default (empty lattice) still parses numeric
`Vector<..., N>` lane counts, so self-contained fixtures are weighted without
passing `--lattice`.

## Driver / reporting

`plugins/freelb/cse_main.cpp` (the generic `cse -c` entry point):

* accumulate `functionCosts` from the first `optimizeRegion` pass and drop the
  duplicate re-optimization loop;
* print `flops / vectorOps / memOps / nodes / stmts / vars` plus
  `unknownLoops` / `unmodeledCalls` warnings in text and JSON;
* `--lattice` selects the lattice for loop dims, lane counts **and** the
  `LatticeResolvePass` alias/name, so `latset::c<LatSet>(k)` is resolved to
  scalar components.

`plugins/freelb/ur_emit.cpp` (the `csegen` generator) reports cost itself:

* `csegen --cost [--json] [--lattice NAME]... <in> <out>` measures
  before/after on the **exact** per-struct pipeline it uses to emit `.ur.h`
  (`lowerVectors`, `CounterPropPass`, and the generator's recombine setting),
  so the numbers cannot drift from the generated code;
* `UrGenerateOptions::report` / `lattices` carry the collection and filter;
* `tools/cse/Makefile` `make cost` drives this, not `cse -c`.

Rationale: a per-call `--lower-vectors` flag on the generic `cse` tool would
have to duplicate `classifyStruct`'s policy and could rot as new struct shapes
appear. Measuring inside the generator keeps a single source of truth.

## Tests

`tests/run_tests.sh` pins post-pass FLOP counts, so the unique-node and
call/vector accounting changes re-baseline those numbers. New fixtures cover
vector arithmetic, nested `dim` loops, descending loops, helper calls,
unknown-bound loops and memory ops. A `csegen --cost` smoke check verifies the
generator's cost path.

## Non-goals

* Full type inference / generic operator overloading semantics — lane rules
  are project hooks.
* Modeling branch probabilities or memory-hierarchy costs.

## Layering

`src/` stays project-agnostic: it contains no FreeLB names or policy. All
domain semantics enter through `CSEConfig` hooks (`vectorLanes`,
`callResultLanes`, `callCost`, `binaryOpCost`, `resolveName`, …) supplied by
`plugins/freelb/config.h`; lattice tables, helper-call weights and the CUDA
token filter live under `plugins/freelb/`. The cost model is behavior-neutral
when those hooks are null (scalar, calls charged 0).
