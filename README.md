# CSE — a small C++ common-subexpression-elimination optimizer

[English](README.md) | [中文](README.zh-CN.md)

CSE analyzes the functions you mark with `//@cse`, finds redundant computation
across statements, and rewrites the region with the duplicates removed. It is a
standalone C++17 tool built on an LLVM-inspired three-stage pipeline —
**Frontend → IR → Passes → Backend** — and ships as a library plus two
executables.

```cpp
//@cse
double compute(double a, double b, double c, double d, double e) {
    double t1 = a * b * c * d * e;
    double t2 = b * c * d;
    double t3 = c * d;
    double t4 = a * b;
    return t1 + t2 + t3 + t4;
}
```

## Highlights

- **Cross-statement CSE on a DAG IR.** Expressions are hash-consed while the IR
  is built, then a dedicated pass extracts common subexpressions that span
  separate assignments.
- **Counted-loop unrolling.** `for (i = 0; i < N; ++i)` is expanded so every
  iteration becomes a statement — the precondition for any cross-statement
  optimization on loop bodies.
- **Safety is opt-in.** `CSEConfig` defaults are conservative — no commutativity,
  no reassociation, no aliasing assumptions, unknown calls treated as impure.
  The `cse` CLI starts from the FreeLB profile (algebraic rules enabled) and
  takes `-s` for the conservative one.
- **Project hooks instead of built-in semantics.** The core `src/` tree contains
  no FreeLB knowledge; everything lattice-specific lives behind `CSEConfig`
  hooks in `plugins/freelb/`.
- **Measurable output.** `-c` reports FLOP counts before and after, `--json`
  emits them machine-readably, and `make test` pins the counts as a regression
  table.

## Pipeline

```
LoopUnroll → [Resolve] → ConstantFold → AlgebraicSimplify → [PostAlgebra]
  → [Reassociate] → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE → Cleanup
```

Bracketed stages are optional or gated by configuration. The pass table, the
plugin slots and the full safety model are documented in
[docs/architecture.md](docs/architecture.md).

## Requirements

- GNU make
- A C++17 compiler (`g++` by default, override with `CXX=...`)
- Python 3 (only for the latset checks inside `make test`)

## Build

```bash
make            # bin/cse, bin/csegen, bin/libcse.a, bin/libcse.so
make test       # build, then run the whole regression suite
make release    # clean rebuild with -O2
make install PREFIX=/usr/local DESTDIR=
make clean
```

## Usage

### Analyze and optimize a `//@cse` region

```bash
./bin/cse input.cpp -c          # report FLOP cost before/after
./bin/cse input.cpp             # optimize; writes input.cpp.cse
./bin/cse input.cpp -r -v       # with expression recombination, per-pass log
./bin/cse input.cpp -s -c --json
```

A region starts at a line that begins with `//@cse` (leading whitespace
allowed) and runs to the end of the next balanced `{ ... }` body. Everything
outside the region is copied through unchanged, and the output file is always
`<input>.cse` next to the source.

| Option | Effect |
|--------|--------|
| `-c`, `--cost` | print FLOP / vector-op / mem-op / node / statement counts (plus coverage) before and after |
| `--json` | emit that cost report as JSON (use with `-c`) |
| `-r`, `--recombine` | enable factor extraction (`a*x + a*y → a*(x+y)`) |
| `-s`, `--safe` | conservative semantics for arbitrary C++ |
| `-v`, `--verbose` | print each pass to stderr |
| `--dump-ir` | print the IR before and after each pass (to stdout) |
| `--verify` | check pass invariants (effect preservation); exit 4 on violation |
| `--lattice NAME` | lattice set for the cost model (D2Q5, D2Q9, D3Q7, D3Q15, D3Q19, D3Q27) |
| `-h`, `--help` | usage |

### Generate FreeLB `.ur.h` specializations

```bash
./bin/csegen tests/csegen/moment.h /tmp/moment.ur.h
./bin/csegen --cost --lattice D3Q19 tests/csegen/moment.h /tmp/moment.ur.h
```

`csegen <input.h> <output.h>` reads a `// @cse`-marked production header and
emits fully unrolled specializations for every supported lattice set
(D2Q5, D2Q9, D3Q7, D3Q15, D3Q19, D3Q27). The input basename must be one of
`moment`, `equilibrium` or `force`.

`--cost` reports before/after FLOP cost measured on the same per-struct
pipeline that emits the file (`--json` for machine output, `--lattice NAME`
repeatable to restrict sets). This is what FreeLB's `make cost` uses, so the
reported cost always matches the generated `.ur.h` — unlike running the generic
`cse -c`, which does not apply the generator's per-shape vector lowering.

Every run also prints a coverage report to stderr — regions, structs, emitted
vs skipped, specialization count — so a struct the generator could not handle
is visible instead of silently dropped:
`csegen: coverage: 7 regions, 7 structs (7 emitted, 0 skipped), 42 specializations`.

FreeLB consumes this through its `third_party/cse` submodule: see
[docs/freelb_port_status.md](docs/freelb_port_status.md) for the integration,
the build entry points and the current status.

## Safety model

In conservative mode (`-s`, and the `CSEConfig` defaults), optimization
preserves behavior through eight mechanisms:

1. **Purity** — calls are assumed to have side effects; only calls registered as
   pure may be deduplicated or hoisted across statements. Any rewrite that
   collapses two occurrences into one requires the collapsed subexpression to be
   pure. The builtin math list matches **qualified** free calls only
   (`std::sqrt`): a bare `max`/`min`/`abs` may name the user's own function, and
   merging two calls to it drops a side effect. Member calls (`obj.sin`) never
   consult the list.
2. **Memory** — loads from read-only roots may be shared; loads from writable or
   unknown roots are kept independent so nothing is reused across a store.
   Pointer *and* reference parameters (`T*`, `T&`, arrays) get no special
   treatment: `const` only promises the pointee is not written *through that
   parameter* — `f(x, x)` may legally bind a `const T&` and a `T&` to the same
   object. Opt in to `noAlias` to share them (the FreeLB profile does, since
   its kernels never alias their arguments).
3. **Store visibility** — variables are interned by name, so reassigning one does
   not create a new node and two identical subexpressions remain the *same* node.
   Cross-statement rewrites therefore check for intervening writes: CSE only
   hoists a subexpression when none of its operands is written between the
   definition point and every later use, and value propagation only inlines an
   initializer whose variables are never written anywhere in the function. An
   element or member store counts as a write to the root of its lvalue, which is
   the conservative answer when another pointer may refer to the same object.
4. **Dominance** — expressions computed inside `if`/`else`/loops are marked
   nested and never lifted out.
5. **Speculation** — `isSpeculatable` (`analysis/purity.h`) answers a different
   question from purity: *may this run where the source did not run it?* It is
   the fix for `k ? 1/k : 0`, whose division used to be hoisted out of the
   conditional and trap at `k = 0`. A guarded operand (a ternary arm, the right
   side of `&&`/`||`) is extracted only when it is speculatable. Hoisting the
   *whole* conditional is always safe — it keeps its internal short-circuit —
   so only the condition / left operand has to be speculatable.
6. **Scopes** — shadowing variables are alpha-renamed so unrelated declarations
   with the same name are never merged. The rename also fires when the region
   *reads* a name it never declares (`double s = t;` where `t` is a global): the
   later `double t = a;` must not become the same node. Every generated name
   (`__s`, `__u`, `_cse_`) comes from `IRModule::freshName`, so it can never
   collide with a user identifier.
7. **Loop unrolling precondition** — a loop is only unrolled when every local it
   declares can be inlined or safely renamed, its induction variable is not
   written by `++`/`--` inside the body, is not read *after* the loop statement,
   and the body contains no `break` / `continue` of its own; otherwise the loop
   stays. The induction variable must also be declared in the `for` init (an
   assignment init keeps the loop).
8. **Bounded recursion** — expression parsing is capped at 512 nesting levels
   and reports `CSE-PARSE-DEPTH` instead of overflowing the stack. The region is
   skipped and passed through, like any other unreadable region.

The code generator also always prints the tree it was given: a right child at
equal precedence keeps its parentheses, because under IEEE-754 not only
`a - (b - c)` and `a / (b * c)` but even `a * (b * c)` differ from the
un-parenthesised form.

Algebraic rules are opt-in per licence: commutativity/associativity via
`assumeNumericCommutative` / `assumeNumericAssociative`, floating-point
reassociation (additive and multiplicative regrouping) via `allowFpReassoc`, and
the special-value / signed-zero identities (`x*0`, `x-x`, `0/x`, `x/x`, `x+0`,
`0-x`) via `allowUnsafeFpIdentities`.

The zero identities substitute a constant, so that constant has to carry the
expression's int/float kind: emitting an integer `0` where the source computed a
double turns an enclosing `/ 4` into integer division. The kind of a load is not
provable, so `assumeFloatOperands` (on in the FreeLB profile, off under `-s`)
says an unprovable operand is floating, which is what lets `0 * cell[i]` fold.
A *proven* integer always wins, so `(0 * k + 2) / 4` stays an integer division.

One rule is unconditionally on because it is bit-exact: sign normalization
(`a * (-b) -> -(a * b)`). Its only deviation is the sign bit of the NaN that
`0 * inf` produces -- not an observable value under IEEE-754, and gating it would
cost the conservative profile the equilibrium kernel's opposite-direction
sharing. `verify_exact_identities.cpp` pins both the exactness and that
exception.

## Tests

`make test` runs `tests/run_tests.sh`:

| Stage | What it checks |
|-------|----------------|
| Cost regression | FLOP counts for the fixtures in `tests/fixtures/`, run under three configurations (default, `-r`, `-s`) |
| Numerical | `verify_*` compile and run generated output (semantics_fixes, equilibrium, safety, recombine, parens, store_aware, float_identities, ref_alias, …) |
| Config contract | `verify_config.cpp` / `verify_counter_prop.cpp` link `libcse.a` and pin the `CSEConfig` switches and plugin-injected passes the CLI cannot isolate |
| Pass contracts | `verify_pass_contracts.cpp` pins that every default-pipeline pass fires somewhere, only LoopUnroll may forgo effect preservation, and cross-statement passes declare the write-set contract |
| Shape checks | grep assertions on generated text, plus `check_stmt_switches.py` (every statement-kind switch stays exhaustive or marked `// partial:`) |
| `csegen` smoke | representative specializations appear in generated headers |
| Lattice drift guard | engine tables vs FreeLB `lattice_set.h` (skipped without a FreeLB checkout) |
| FreeLB verifiers | `verify_{moment,equilibrium,force}.py` (skipped without a FreeLB checkout) |

The cost model counts **scalar FLOPs with vector ops weighted by lane count**
(a vector-vector product is a dot: `2d-1`), charges known pure helpers via a
FreeLB hook (`getnorm2` → `2d-1`, …), and charges each **reference** to a DAG
node — once per use site, so a shared node that codegen re-prints costs each
time it is printed, while a CSE-materialized temporary costs only its
definition. `totalNodes` stays the distinct-node count. It reports
`unknownLoops` / `unmodeledCalls` so unresolved constructs are visible as a
lower bound. See `docs/cost_model_redesign.md`.

Sample results (before → after): `basic_cse` 11 → 10, `features` 52 → 51,
`equilibrium_d3q19` 323 → 84 FLOPs (−74.0%), `safety_cases` 24 → 21,
`cost_nested` 478 → 355 (conservative since `const` parameters stopped being
assumed read-only roots), `cost_descending` 35 → 35.

## Repository layout

```
src/            generic frontend, IR, passes, backend, cost model
plugins/freelb/ FreeLB config hooks, latset resolution, .ur.h emitter,
                and the two program entry points
tests/fixtures/ //@cse inputs pinned to expected FLOP counts
tests/verify/   numerical verifiers and the lattice drift guard
tests/csegen/   .ur.h generation smoke inputs
docs/           architecture and port status
```

## Documentation

- [docs/architecture.md](docs/architecture.md) — pipeline, IR, passes, safety
  model, build and test coverage
- [docs/cost_model_redesign.md](docs/cost_model_redesign.md) — the cost
  accounting model (per-reference FLOPs, vector weighting, hooks)
- [docs/freelb_port_status.md](docs/freelb_port_status.md) — FreeLB integration
  status, open TODOs and the generic/FreeLB boundary
- [docs/cse-round6-audit.md](docs/cse-round6-audit.md) — the latest full
  architecture audit (defects found, fixes landed, backlog dispositions;
  not version-controlled)

## Known limitations

Type checking and template instantiation are out of scope (types are strings),
there is no error recovery in the parser, optimization is confined to marked
regions, and only counted loops with constant bounds are unrolled. The full
list lives in [docs/architecture.md](docs/architecture.md).

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
