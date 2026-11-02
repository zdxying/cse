# AGENTS.md — guide for AI coding agents

CSE is a standalone C++17 common-subexpression-elimination optimizer for C++. It
optimizes the functions/declarations marked `//@cse` and writes the result next
to the input as `<input>.cse`. One library plus two executables (`cse`,
`csegen`), no dependencies beyond the standard library. It is also the engine
behind FreeLB's generated `.ur.h` headers, integrated entirely in
`plugins/freelb/`.

## Language rules

Code files — comments, identifiers, diagnostics, commit messages — are entirely
in English. Answer the user in the language they asked in.

## Build and run

```bash
make            # bin/libcse.a, bin/libcse.so, bin/cse, bin/csegen
make test       # build, then tests/run_tests.sh
make sanitize   # clean ASan/UBSan build + the same suite
make clean
```

- `Makefile` is the only build file. Default flags
  `-std=c++17 -Wall -Wextra -g -O0 -Isrc`; the build is warning-clean and must
  stay that way. `CXX=` overrides the compiler.
- `make sanitize` rebuilds everything with ASan/UBSan and leaves instrumented
  objects in `bin/`. Run `make clean && make` before a plain `make test`, or the
  verifiers link an ASan `libcse.a` without the runtime and fail to link.
- CI (`.github/workflows/ci.yml`) runs `make test` and `make sanitize`.

CLI options are tabulated in `README.md`. Exit codes of `cse` are a contract
(`plugins/freelb/cse_main.cpp`): `0` all regions readable, `2` some passed
through, `3` none readable, `4` internal invariant or `--verify` violation. An
unsupported but marked region (declaration, preprocessor directive) is a legal
no-op and does not change the exit code. Output is written at the very end, so a
normally returning process always leaves an output file.

`csegen` requires its input basename to be `moment.h`, `equilibrium.h`, or
`force.h`, and has no passthrough: a region it cannot regenerate fails the run.

## Architecture

```
src/{frontend,ir,passes,analysis,backend}   plugins/freelb   tests   docs
Frontend (lexer/parser/AST) -> IR (DAG) -> Passes -> Backend (CodeGen)
```

Each `//@cse` region runs the pipeline independently. Regions are cut from the
lexer's token stream, so braces or `#` inside comments and string literals cannot
move a boundary; `//@cse` and `// @cse` are both accepted.

The IR is a **hash-consed DAG of expressions plus a structured statement tree**,
not a CFG (`StmtIRKind`: `ExprStmt`, `Assign`, `VarDecl`, `ForLoop`, `IfElse`,
`Block`, `Return`). Default order (`PassManager::createDefault`):

```
LoopUnroll -> [Resolve] -> ConstantFold -> AlgebraicSimplify -> [PostAlgebra]
  -> [Reassociate] -> CSEPass -> [ExprRecombine -> AlgebraicSimplify]
  -> ValueProp -> DCE -> Cleanup
```

`Resolve`/`PostAlgebra` are the plugin slots. `Reassociate` needs
`assumeNumericAssociative && allowFpReassoc`; `ExprRecombine` needs
`enableRecombine && allowFpReassoc`.

Two invariants decide most changes:

1. **`src/` is a generic engine with zero FreeLB semantics** — no
   freelb/lattice/latset content, no `#include` of `plugins/`. Convention only,
   no automated check. Domain behavior enters through `CSEConfig` hooks
   (`resolveName`, `isPureFunction`, `noAlias`, vector lowering, cost hooks) and
   the two pass slots; plugin passes live in `plugins/freelb/`. Before adding a
   capability, ask whether it can be a hook or a slot.
2. **Interned IR nodes are never mutated.** Build through an `IRModule` factory
   (`fill -> recomputeHash() -> intern()`); rewrite via `rebuildWithOperands`
   (`src/ir/ir_utils.h`). Node identity is exactly
   `CSE_DAGNODE_IDENTITY_FIELDS` (`src/ir/dag_node.h`) — a semantic field missing
   there is silently excluded. `IRModule::verify()` checks the invariants after
   every pass in debug builds.

## Safety model — read before touching a pass

The optimizer must never change program behavior. Each rule below exists because
its absence produced wrong code.

- **Conservative by default.** Calls have side effects unless proven otherwise;
  the math whitelist applies only to *qualified free* calls (`std::sqrt`), never
  to bare `max`/`min`/`abs` or member calls (`IRBuilder::isPureCallee`). Any
  collapse of two occurrences into one needs the subexpression to be deep-pure:
  `isReferentiallyTransparent` (`src/analysis/purity.h`), not the per-node `pure`
  flag, which does not propagate to parents.
- **One owner for purity.** `analysis/purity.h` defines `hasObservableEffect`,
  `isReferentiallyTransparent`, `isEffectFreeLvalue`, `isLeafValue`,
  `isCheapShareableValue`, `isSpeculatable`. Do not write a local variant.
- **Write sets.** Cross-statement rewrites consult
  `AliasInfo::build(...).writtenBy(stmt)` (`src/ir/location.h`), never a
  hand-built walker. Element/member writes count against the lvalue root. A pass
  reusing computation across statements must override
  `Pass::crossesStatements()` and actually check writes.
- **Aliasing.** `const T*` / `const T&` are not read-only roots. The only opt-in
  for sharing such loads is `CSEConfig::noAlias` (the FreeLB profile sets it,
  `-s` clears it).
- **Speculation is not duplication.** A guarded subexpression (ternary arm, right
  side of `&&`/`||`) may be hoisted only when `isSpeculatable`; otherwise
  `k ? 1/k : 0` traps at `k == 0`. Hoisting the whole conditional is fine.
- **FP identities are tiered.** Bit-exact ones (`x-0 -> x`, `x*1 -> x`,
  `x*2 -> x+x`) always run; commutativity/associativity need `assumeNumeric*`;
  reassociation needs `allowFpReassoc`; special-value/signed-zero ones (`x*0`,
  `x-x`, `0/x`, `x/x`, `x+0`, `0-x`) and Cleanup's chained-additive merge need
  `allowUnsafeFpIdentities`.
- **No type system.** Types are strings. Never fold division by zero, never
  rewrite division as multiplication, keep a right child's parentheses at equal
  precedence (`a*(b*c) != (a*b)*c` under IEEE-754), keep int/float kind
  (`4.0/3` vs `4/3`), never emit a non-finite literal.
- **Generated names** (`__s`, `__u`, `_cse_`) come only from
  `IRModule::freshName`/`reserveName`.
- **One expression-slot list.** `forEachExpr`/`forEachExprDeep`
  (`src/ir/stmt_walk.h`) own every slot; a new statement kind or slot goes there.
  `CSEPass` walks with `includeLvalue=false` (a write target must not become a
  temporary).
- **Loop control** is a bare-variable `ExprStmt` named `break`/`continue`;
  `isLoopControl` owns that test. Unrolling refuses a loop whose induction
  variable is written in the body, read after it, or whose body has its own
  `break`/`continue`, and refuses a local it cannot inline or rename safely.
- **Statement invariants.** `AssignIR` sets exactly one of `target`/`targetExpr`
  (`hasExactlyOneTarget()`); a compound store keeps `compoundOp` rather than
  lowering, so a side-effecting lvalue is evaluated once.
- **Effect preservation.** Every pass returns true from `preservesEffects()`
  except `LoopUnroll`. `--verify` and the effect oracle compare the multiset
  *and* relative order of impure calls and increments/decrements
  (`src/analysis/effects.h`).
- **Errors are typed.** Frontend failures throw `CSEError` with
  `Diagnostic{phase, code, line, col, message}` and a stable code
  (`CSE-PARSE-EXPECT`, `CSE-PARSE-DEPTH`, ...); never a bare
  `std::runtime_error`. A region that fails to parse is passed through and
  counted as skipped — not an error. A silent no-op must never look like
  success: report through enum results and exit codes.

## Coding style

- Switches over `StmtIRKind` list every enumerator or carry `// partial:` on the
  `default:` line; `tests/verify/check_stmt_switches.py` enforces it.
- Includes: relative inside `src/` (`"../ir/ir_module.h"`); rooted at `src/` from
  `plugins/`, `tests/verify/` and `run_tests.sh` (the build passes `-Isrc`). A
  `.cpp`'s first include is its own header.
- Comments explain *why* and the invariant behind the code, with a summary on
  every class/function — not what a line literally does.
- Determinism: break sort ties by `node->id`; never let `unordered_map`
  iteration order decide emitted code.
- Ownership: `std::unique_ptr`, non-owning raw pointers in the DAG, `const&`
  parameters. Use `= delete` to make a dangerous call a compile error.
- `.clang-format` is Google-based, `ColumnLimit: 90`.

Naming: `PascalCase` types, `camelCase` functions/variables, `_underscore`
private members, `kName` constants, `ALL_CAPS` macros. Everything lives in
`namespace cse`, plugins in `cse::freelb`.

## Tests

`make test` runs `tests/run_tests.sh`; all artifacts go to a `mktemp -d` and the
source tree is untouched. There is no unit-test framework: verifiers are
standalone C++ programs that print a banner, which the script greps while
checking exit codes. `tests/README.md` is the authoritative fixture/verifier
inventory.

Oracles: (1) cost pins in `cost_stage` across default/`-r`/`-s`, FLOPs charged
per reference; (2) numerical `sf_X` vs `ref_X` twins, `-s` tightening `checkNear`
from 1e-9 to 1e-12; (3) sweep oracles over every region (round-trip, idempotence,
effect preservation, pass contracts, IR invariants); (4) shape greps for what
values cannot see (parentheses, `-0.0`, `break;` survival); (5) twin differential
plus 32-seed fuzzing; (6) class-level gates (token census, coverage, outcome
contract, `-fsyntax-only` over outputs, switch exhaustiveness); (7) library-level
verifiers linking `bin/libcse.a`; (8) optional FreeLB checks, skipped without a
checkout.

Adding a fixture or a pass: follow `tests/README.md`; for a pass also update
`verify_pass_contracts.cpp` and `verify_ir_idempotence.cpp`. A pass that is wired
in but never fires is a test failure by design. FLOP pins are blind to missing
parentheses and effect defects, so behavioral changes need a numerical or shape
oracle. Re-measure any pin with `bin/cse <fixture> -c` and explain the delta.

## Known limitations

The full table is in `docs/architecture.md`. The entries that most often mislead
a change: types are strings (no checking, instantiation, or deduction); only
marked regions are optimized and there is no in-region error recovery; only
constant-bound counted loops unroll (max 64) and `if`/`else` bodies are excluded
from cross-statement CSE; preprocessor directives are not modeled; CSE extraction
is O(n²); nesting is capped at 512 (`CSE-PARSE-DEPTH`); `a[i] += x` keeps
`compoundOp` instead of expanding; deferred refactors and their restart
conditions live in `docs/architecture.md` and `docs/cse-round6-audit.md`.

## Documentation map

`README.md` / `README.zh-CN.md` — user-facing overview and options.
`docs/architecture.md` — pipeline, passes, limitations (Chinese).
`docs/cost_model_redesign.md` — cost accounting.
`docs/freelb_port_status.md` — FreeLB boundary and status.
`tests/README.md` — fixture/verifier inventory (Chinese).
`docs/cse-round6-audit.md` — latest audit, not version controlled.
