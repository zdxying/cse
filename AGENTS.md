# AGENTS.md — working on CSE

CSE is a standalone C++17 common-subexpression-elimination optimizer for C++. It
rewrites `//@cse`-marked functions/declarations and writes `<input>.cse`: one
library (static and shared), two executables (`cse`, `csegen`), no dependencies
beyond the standard library (`make test` also needs Python 3). It is also the
engine behind FreeLB's generated `.ur.h` headers, integrated in `plugins/freelb/`.

This file holds only what the code will not tell you. Read `docs/architecture.md`
before touching `src/`; the fixture inventory is `tests/README.md`. If this file
disagrees with the code, the code wins for local style — report the mismatch.

## 1. How to work

- Author decides what to build, the acceptance criterion and the trade-offs; the
  agent plans, codes, and quotes real command output as evidence.
- Before a non-trivial edit, state the plan in five lines: which files change,
  which §3 row you will run, how you will know it worked.
- Change what the task needs: no drive-by refactors, reformatting or renames. A
  new abstraction is fine if justified; every diff line should answer which
  requirement it serves, or be reverted.
- Find the cause before naming it; a familiar error can have a different cause.
  Separate confirmed from hypothesis, and fix only what was asked. Say what you
  could not verify: "not tested" is fine, "should work" is not.
- Code, comments, diagnostics and commit messages in English; answer the author in
  the language of the question.
- Done means: exercised by a real run (not just compiled), the §3 row passed, new
  behaviour covered by a test, comments updated, stale docs reported.
- Match the file you edit; `.clang-format` is a fallback, not a licence to
  reformat. `PascalCase` types, `camelCase` functions, `_underscore` members,
  `kName` constants; everything in `namespace cse`, plugins in `cse::freelb`.
- Related docs: `README*.md` user-facing, `docs/architecture.md` design and known
  limitations, `docs/cost_model_redesign.md` accounting,
  `docs/freelb_port_status.md` boundary, `tests/README.md` inventory.

## 2. Invariants that fail silently

Each guards against wrong code or a lost optimisation; mechanism is in the header.

**Boundary** — `src/` is a generic engine with zero FreeLB semantics: no
FreeLB-specific types, no `#include` of `plugins/`. Domain behaviour enters
through `CSEConfig` hooks and the two pass slots; ask whether a capability can be
a hook or a slot before adding it.

**Interning** — node identity is exactly `CSE_DAGNODE_IDENTITY_FIELDS`
(`src/ir/dag_node.h`); a semantic field missing there is silently excluded from
dedup, and `sameContentAs`/`computeHash` both expand the macro — never edit one
alone. Interned nodes are never mutated: build through an `IRModule` factory
(fill -> `recomputeHash()` -> `intern()`), rewrite through `rebuildWithOperands`
(`src/ir/ir_utils.h`); `IRModule::verify()` checks it after every pass in debug.

**Purity and effects** — `src/analysis/purity.h` owns the questions
(`hasObservableEffect`, `isReferentiallyTransparent`, `isSpeculatable`,
`isEffectFreeLvalue`, `isCheapShareableValue`); do not write a local variant.
`DAGNode::pure` is per-node and does not propagate, so collapsing two occurrences
requires the *deep* `isReferentiallyTransparent`. Calls are impure unless proven
otherwise; the math whitelist matches **qualified** free calls only (`std::sqrt`),
never bare `max`/`min`/`abs` or member calls. Every pass preserves effects except
`LoopUnroll`; `--verify` and `src/analysis/effects.h` compare the multiset and
relative order of impure calls and increments.

**Speculation is not duplication** — a guarded operand (ternary arm, right side of
`&&`/`||`) may be hoisted only when `isSpeculatable`, or `k ? 1/k : 0` traps at
`k == 0`. Expressions inside `if`/`else`/loops are nested and never lifted out.

**Memory and aliasing** — cross-statement rewrites consult
`AliasInfo::build(...).writtenBy(stmt)` (`src/ir/location.h`), never a hand-built
walker; element/member writes count against the lvalue root. A pass reusing
computation across statements overrides `Pass::crossesStatements()` and checks
writes. `const T*` / `const T&` are not read-only roots: `f(x, x)` may bind both;
only `CSEConfig::noAlias` (the FreeLB profile) licenses sharing them.

**Slots and statements** — every expression slot is listed once, in
`forEachExpr`/`forEachExprDeep` (`src/ir/stmt_walk.h`); a new kind or slot goes
there. `CSEPass` walks with `includeLvalue=false`: a write target must not become
a temporary. `AssignIR` sets exactly one of `target`/`targetExpr`, and a compound
store keeps `compoundOp` so a side-effecting lvalue is evaluated once.

**Names and kinds** — generated names (`__s`, `__u`, `_cse_`) come only from
`IRModule::freshName`/`reserveName`. Types are strings: never fold division by
zero, never rewrite division as multiplication, keep a right child's parentheses
at equal precedence (`a*(b*c) != (a*b)*c` under IEEE-754), keep int/float kind
(`4.0/3` vs `4/3`), never emit a non-finite literal. The zero identities
substitute a constant whose kind must match the expression; `assumeFloatOperands`
licenses an unprovable operand as floating, and a proven integer wins.

**Control flow and limits** — loop control is a bare-variable `ExprStmt` named
`break`/`continue`; `isLoopControl` owns that test. Only counted loops with
constant bounds unroll (max 64); unrolling refuses a loop whose induction variable
is written in the body or read after it, whose body has its own `break`/`continue`,
or whose locals cannot be inlined or renamed safely. Directives are not modelled,
so a region containing one is passed through. Nesting is capped at 512
(`CSE-PARSE-DEPTH`): a deeper region is skipped, never a crash.

**Reporting** — errors are typed: `CSEError` with
`Diagnostic{phase, code, line, col, message}` and a stable code
(`CSE-PARSE-EXPECT`, `CSE-PARSE-DEPTH`, ...), never a bare `std::runtime_error`. A
failed region is passed through and counted as skipped, not an error. A silent
no-op must never look like success: report through enum results and exit codes — 0
all readable (unsupported is a legal no-op), 2 some unreadable, 3 none readable, 4
internal invariant or `--verify`. Switches over `StmtIRKind` list every enumerator
or carry `// partial:` on `default:` (`tests/verify/check_stmt_switches.py`).
Determinism: break sort ties by `node->id`, never by `unordered_map` order.

## 3. Tests and gates

`make test` runs `tests/run_tests.sh`; artifacts go to a `mktemp -d`, the source
tree is untouched. Verifiers are standalone programs with an authoritative exit
code; some print a banner the harness checks. `tests/README.md` is the inventory.

| Change | Required |
| --- | --- |
| `docs/`, `README*.md` | no run; commands and links must resolve |
| one pass, or a pass rule | `make test`, plus `verify_pass_contracts` and `verify_ir_idempotence` |
| frontend lexer/parser | `make test`; `verify_lexer` and the `frontend_*` fixtures |
| IR node, identity, slots | `make test`; `verify_node_identity`, `verify_ir_invariants` |
| purity, write sets, aliasing | `make test`; `verify_purity`, `verify_write_set` |
| cost model | `make test`; `verify_cost`; re-baseline pins and explain the delta |
| `plugins/freelb`, `csegen` | `make test`; `tests/csegen` smoke, `csegen --cost`; FreeLB verifiers when a checkout exists |
| a new fixture | register it in `tests/README.md` and `run_tests.sh` with a cost pin or a verifier |
| before calling anything done | `make test` all green, then `make sanitize` (ASan/UBSan) |

- A pass wired in but never firing is a test failure by design.
- FLOP pins are blind to shape and order: missing parentheses, reordered effects,
  `-0.0`, pre/post `y++`. A behavioural change needs a numeric or shape oracle.
- Re-measure any pin with `bin/cse <fixture> -c` and say why it moved.
- `make sanitize` leaves instrumented objects in `bin/`; run `make clean && make`
  before a plain `make test`, or the verifiers fail to link.
- Missing tool? Run what you can and say which row was not met; never skip a gate.

## 4. Changing numerics

- FP identities are tiered: bit-exact ones (`x-0 -> x`, `x*1 -> x`, `x*2 -> x+x`)
  always run; commutativity/associativity need `assumeNumeric*`; reassociation
  additionally needs `allowFpReassoc`; the special-value/signed-zero identities
  (`x*0`, `x-x`, `0/x`, `x/x`, `x+0`, `0-x`) and Cleanup's chained-add merge need
  `allowUnsafeFpIdentities`.
- Reordering a sum in a hot loop is a numeric change, not a cleanup: say so and add
  the oracle. A cost-model change is not neutral either — quote the old and new pin
  and the command that produced them.
- Performance claims need the machine, the command and before/after numbers;
  neutral is a valid result.

## 5. Docs and commits

- Architecture, not implementation: the invariant, the boundary, the reason.
  Mechanism lives in the code; keep the pointer. References go one way — docs point
  to code, code comments carry their own reason.
- A change updates the docs it invalidates (README option table, architecture cost
  table) in the same commit; anything beyond that is a separate `docs:` commit.
- A doc states the present, not its history — git carries the changelog. Editing a
  doc is when you prune.
- Commit: `type(scope): imperative summary` (lowercase, no period), then bullets of
  why/what/how, then what you verified with which command. `type` follows the log:
  feat, fix, perf, refactor, test, docs. English, no AI trailer. Report stale
  claims you did not fix.

**This file's own rule**: a line earns its place only if ignoring it produces a
silent wrong result or an unmet gate. Mechanism and narrative belong in the doc it
points to. Keep it to ~170 lines; when it grows, delete before adding.

## 6. Ask the author first

1. A new dependency, submodule or vendored code.
2. Deleting or renaming a public header, an executable, a fixture or a verifier.
3. Any `git commit` / `push` / branch / tag / history operation.
4. Changing a default, an exit code or a numeric result.
5. Changing a format other tools consume (`.cse`, `.ur.h`, `--json`).
6. Weakening a test or skipping a gate row.
