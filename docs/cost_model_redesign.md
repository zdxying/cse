# CSE Cost Model

`analyzeCost` (`src/analysis/cost_model.{h,cpp}`) estimates the work the
generated code executes for one IR module. It is analysis-only: it does not
change the IR or select passes. `flops`, `vectorOps` and `memOps` are charged
**per reference** to a DAG node, scaled by the number of executions of the
enclosing loop context; `totalNodes` remains the distinct-node count. That
distinction matters because codegen re-prints a shared, unmaterialized node at
every use site, while a CSE-materialized temporary reaches the IR as one
definition whose uses are cheap variable references.

## Result

`CostResult` (`src/analysis/cost_model.h`) aggregates one module:

| Field | Meaning |
|-------|---------|
| `flops` | scalar FLOPs, vector ops weighted by lane count |
| `vectorOps` | operations with at least one vector operand |
| `memOps` | loads (array/member/arrow) per reference, plus one per complex-lvalue store |
| `totalNodes` | distinct DAG nodes reached, not reference-weighted |
| `stmts` | executed statements, loop-scaled |
| `vars` | executed declarations, loop-scaled |
| `unknownLoops` | loops whose iteration count could not be resolved |
| `unmodeledCalls` | calls with no cost-model entry |

`unknownLoops` and `unmodeledCalls` count distinct constructs, not references.
A non-zero value marks an approximation; `flops` is then a lower bound
(`src/analysis/cost_model.h`).

## Counting rules

Statements are visited in program order. Every expression slot contributes one
reference to its node per execution of the current loop context. The references
to a node are summed and propagated to its operands
(`mult[operand] += mult[parent]`), so a shared subgraph is expanded
arithmetically instead of re-walked. Each reachable node is then charged
`cost(node) × mult[node]` in parents-before-children topological order
(O(V+E)). `if`/`else` sums both branches (worst case).

Per-node cost:

* `BinaryOp` with `+ - * /`: FLOPs from `binaryOpCost` (which writes the FLOP
  count and returns the result lane count); without the hook, 1 FLOP and a
  scalar result. `vectorOps` is incremented per reference when either operand
  has a positive lane count.
* `UnaryOp` `+`/`-` and `++`/`--`: lane-count FLOPs (vector) or 1. An
  increment/decrement is one add/subtract of the constant 1; `!`/`~` are not
  charged. `vectorOps` is incremented when the operand has a positive lane
  count.
* `ArrayAccess`/`MemberAccess`/`ArrowAccess`: one mem op per reference.
* `Assign` with a complex lvalue (`targetExpr`): one mem op for the store per
  execution. The lvalue's base/index operands are counted as references, not as
  an extra load.
* `Call`: `callCost(callee, argLanes)`; a non-negative result adds FLOPs, a
  negative one charges 0 and increments `unmodeledCalls` once for that node.
  Without the hook a call costs 0 and is not flagged. The callee expression slot
  is not traversed.
* `Ternary`/`Cast`: no direct cost; their operands are counted normally.

## Lane inference

`vecDim` is attached by `IRBuilder` to parameters and declared locals from their
declared types through `CSEConfig::vectorLanes` (`src/ir/ir_builder.cpp`). It is
derived metadata and is not part of the structural hash
(`CSE_DAGNODE_IDENTITY_FIELDS` in `src/ir/dag_node.h`), so it does not affect CSE
deduplication.

`LaneAnalyzer` (`src/analysis/cost_model.cpp`) computes lanes for compound
expressions on the fly:

* `Variable` → stored `vecDim`; if absent, a forward pass (`inferVarLanes`)
  infers lanes from the variable's initializer or assigned value in program
  order. CSE places definitions before uses, so one pass suffices.
* `Call` → `callResultLanes(callee)`, else
  `isVectorProducingCall(callee) ? vectorDim : 0`.
* `BinaryOp` → the result lane count returned by `binaryOpCost`; scalar without
  the hook.
* `UnaryOp`/`Cast` → the operand's lanes.
* `Ternary` → the maximum of the two arms' lanes.

FreeLB (`plugins/freelb/config.h`) supplies the lane hooks:

* `vectorLanes` parses the numeric size of `Vector<..., N>`; a non-numeric size
  falls back to the configured lattice dimension.
* `callResultLanes` treats only lattice direction accessors
  (`latset::…::c`, `lattice::…::c`) as vector-valued: it parses `D<n>` from the
  callee, else returns the lattice dimension. Other callees are scalar.

## Loop iteration counting

A stack of `LoopDesc {var, start, bound, step, inclusive}` is maintained during
traversal. Counted loops are recognized in both directions — ascending
(`<`/`<=` with `++`/`+=k`) and descending (`>`/`>=` with `--`/`-=k`) — and the
step sign must agree with the comparison direction.

`execCount` returns the number of executions of the current loop context:

* **Rectangular nests** take a fast path: when no loop extent references an
  enclosing loop variable, the count is the product of the per-loop trip counts.
* **Dependent nests** (e.g. the triangular `for (b = a; b < N; ++b)` inside
  `a`) are enumerated exactly, binding each loop variable in the environment:

  ```
  execCount(stack, i):
    if i == len(stack): return 1
    total = 0
    for v in start_i .. bound_i (respecting step_i and inclusive_i):
        bind(var_i, v); total += execCount(stack, i + 1)
    return total
  ```

  The product probe caps enumeration at `kMaxEnum` (1e6). A nest over the cap
  stops at the overflow point and returns the partial product instead of
  enumerating; the loop is counted in `unknownLoops` so the approximation is
  visible.

A loop that `recognizeLoop` rejects, or whose start/bound references a variable
not bound by an enclosing loop, is charged once per execution of the enclosing
context and increments `unknownLoops`.
The counter is incremented once per loop construct, not once per statement slot.
When `unknownLoops > 0`, `flops` is a lower bound on the real cost — the
contract documented by `CostResult` and printed by the CLI.

## Hooks

The model reads all project semantics from optional hooks in
`src/frontend/cse_config.h`:

| Hook | Meaning |
|------|---------|
| `vectorLanes(type)` | declared type → lane count |
| `callResultLanes(callee)` | callee → result lane count |
| `callCost(callee, argLanes)` | scalar FLOP cost; negative = unmodeled |
| `binaryOpCost(op, aLanes, bLanes, flops)` | writes FLOPs, returns result lanes |

`plugins/freelb/config.h` wires them from the active `LatticeConfig`:

* `callCost`: `getnorm2 → 2d-1`, `getnorm → 2d`, `getsum → max(0, d-1)`,
  `dot → 2d-1`, `latset::*`/`lattice::*`/`clear`/`get*` → 0, builtin scalar math
  (`sqrt`, `pow`, …) → 1, otherwise `-1`. Here `d` is the larger of the lattice
  dimension and the argument lane counts. Vector-valued
  `getnorm*`/`dot`/`getsum` need `d > 0` (from a lattice dimension or a numeric
  `Vector<..., N>`); with no lane information they are unmodeled (`-1`).
* `binaryOpCost`: `*` is a dot product (`2d-1`, scalar result) when both operands
  are vectors, otherwise scaling by the lane count; `+ - /` are componentwise
  (max lane count).

The generic `createFreeLBConfig()` default (empty lattice) still parses numeric
`Vector<..., N>` lane counts, so self-contained fixtures are weighted without
passing `--lattice`.

## Boundary

Lattice tables and helper-call weights live in `plugins/freelb/config.h`; the
cost model reaches them only through `CSEConfig` hooks. With null hooks it is
scalar and calls cost 0 without being flagged.

## Verification

`tests/run_tests.sh` pins post-pass FLOP counts for a set of fixtures, with
default, `-r` and `-s` stages. `tests/verify/verify_cost.cpp` pins the
per-reference rule on shapes where it differs from unique-node counting,
including a shared node across statements, a duplicate operand in one statement,
a materialized temporary, loop scaling and an over-cap loop.
`tests/verify/verify_cost_descending.cpp` pins descending/stepped loops. The
`cost_nested` fixture covers a triangular nest, an unresolved bound, vector
weighting and a helper call; `cost_descending` covers negative steps. A
`csegen --cost` smoke check verifies the generator's cost path. The fixture
inventory is `tests/README.md`.

The CLI and generator print these fields; their wiring (`cse -c`,
`csegen --cost`, `make cost`) is documented in `README.md` and
`docs/architecture.md`.

## Non-goals

* Full type inference / generic operator-overloading semantics — lane rules are
  project hooks.
* Modeling branch probabilities or memory-hierarchy costs.
