#!/usr/bin/env python3
"""Deterministic generator of twin programs for the write-set fuzzer.

The curated shape table in `verify_write_set.cpp` still needs a human to add a
row for each new statement shape. This generator closes that gap: it draws
programs from a grammar of the write-set shapes -- declarations, assignments,
compound stores, reference declarations and chains, nested blocks, if/else,
counted loops, repeated subexpressions, local arrays with element
stores/loads, and non-counter `for`-update increments -- and emits them as
`sf_`/`ref_` twin pairs with identical bodies. The twin differential then runs
the real pipeline on the marked `sf_` and compares the value against the
untouched `ref_`, bit for bit. A shape the analyzer fails to enumerate, or a
pass that forwards a stale value across a write it did not see, shows up as a
mismatch without anyone having thought of that shape first.

    gen_shapes.py <seed> <functions> <out.cpp>

Scalar signature by construction: the twin harness (gen_twin_main.py) exercises
only scalar signatures, so arrays are always locals of the function, never
parameters. Programs the frontend cannot read are skipped by the caller, never
failed -- the generator is allowed to be imperfect, the oracle is not.
"""

import random
import sys

LEAVES = ["1.0", "2.0", "3.0", "0.5"]
OPS = ["+", "-", "*"]


def gen_expr(rng, scope, depth):
    """A pure arithmetic expression over in-scope variables and constants."""
    if depth <= 0 or rng.random() < 0.30:
        return rng.choice(scope["vars"] + LEAVES)
    if scope["exprs"] and rng.random() < 0.25:
        # Reuse a whole subexpression verbatim: gives CSE / ValueProp something
        # to share, and -- when a variable it reads is written in between -- the
        # exact hazard the write-set analysis exists to prevent.
        return rng.choice(scope["exprs"])
    op = rng.choice(OPS)
    e = "(" + gen_expr(rng, scope, depth - 1) + " " + op + " " + \
        gen_expr(rng, scope, depth - 1) + ")"
    return e


def fresh(counter):
    counter[0] += 1
    return counter[0]


def copy_scope(scope):
    return {"vars": list(scope["vars"]), "exprs": list(scope["exprs"]),
            "arrays": list(scope["arrays"])}


def gen_stmt(rng, scope, depth, out, counter):
    choices = ["decl", "assign", "compound", "ref"]
    if scope["arrays"]:
        # Element stores through an in-scope local array. Gated on an array
        # existing: the store is the shape, and without a declaration in scope
        # it would not compile.
        choices += ["store"]
    if depth > 0:
        choices += ["block", "if", "loop", "eloop", "updinc"]
    kind = rng.choice(choices)

    if kind == "decl":
        name = "v%d" % fresh(counter)
        e = gen_expr(rng, scope, 2)
        scope["exprs"].append(e)
        out.append("  double %s = %s;" % (name, e))
        scope["vars"].append(name)

    elif kind == "assign":
        t = rng.choice(scope["vars"])
        out.append("  %s = %s;" % (t, gen_expr(rng, scope, 2)))

    elif kind == "compound":
        t = rng.choice(scope["vars"])
        op = rng.choice(["+=", "-=", "*="])
        out.append("  %s %s %s;" % (t, op, gen_expr(rng, scope, 2)))

    elif kind == "ref":
        target = rng.choice(scope["vars"])
        r = "r%d" % fresh(counter)
        out.append("  double& %s = %s;" % (r, target))
        scope["vars"].append(r)
        # Leave the reference unwritten part of the time: a referent that is
        # never reassigned is exactly the shape that tempted an inliner to
        # substitute its value into the binding (`double& r = 0.5;`).
        if rng.random() < 0.7:
            wop = "=" if rng.random() < 0.5 else "+="
            out.append("  %s %s %s;" % (r, wop, gen_expr(rng, scope, 1)))
        if rng.random() < 0.3:
            s = "s%d" % fresh(counter)
            out.append("  double& %s = %s;" % (s, r))
            scope["vars"].append(s)
            if rng.random() < 0.5:
                out.append("  %s = %s;" % (s, gen_expr(rng, scope, 1)))

    elif kind == "block":
        out.append("  {")
        gen_body(rng, copy_scope(scope), depth - 1, out, counter)
        out.append("  }")

    elif kind == "if":
        out.append("  if (%s > 0.0) {" % gen_expr(rng, scope, 1))
        gen_body(rng, copy_scope(scope), depth - 1, out, counter)
        out.append("  } else {")
        gen_body(rng, copy_scope(scope), depth - 1, out, counter)
        out.append("  }")

    elif kind == "loop":
        i = "i%d" % fresh(counter)
        k = rng.randint(1, 3)  # literal bound: the unroller can expand it
        out.append("  for (int %s = 0; %s < %d; ++%s) {" % (i, i, k, i))
        gen_body(rng, copy_scope(scope), depth - 1, out, counter)
        out.append("  }")

    elif kind == "store":
        # `arr[k] = expr;` -- an element store through a literal index (the
        # frontend's counted-loop path indexes with the loop counter; a literal
        # keeps the store visible in straight-line code too). The write must be
        # in the closed write set as a write of `arr`, which is exactly what
        # 51a4b6b / a898e7c pinned after the analysis missed it.
        name, size = rng.choice(scope["arrays"])
        idx = rng.randint(0, size - 1)
        # Store first, load after: an element must be written before it is read
        # (the array is uninitialized, so a pre-store load is undefined
        # behaviour in the generated program, and the two twins may then
        # legitimately differ -- a generator bug, not an optimizer one. Seed 5
        # found this the hard way: `p2 = arr1[5]; arr1[5] = 0.5;` reads garbage,
        # and the garbage differs between the twin and its copy).
        out.append("  %s[%d] = %s;" % (name, idx, gen_expr(rng, scope, 2)))
        # A load after the store, registered as a shared subexpression: a later
        # statement reusing it is exactly "a value reused across an element
        # store", the class the closed-write-set analysis must refuse. The
        # hazard needs both sides; a store nothing reads back cannot expose a
        # stale forwarding.
        if rng.random() < 0.7:
            e = "%s[%d]" % (name, idx)
            scope["exprs"].append(e)
            vn = "v%d" % fresh(counter)
            out.append("  double %s = %s;" % (vn, e))
            scope["vars"].append(vn)
    elif kind == "eloop":
        # A loop that declares an array and fills it, then reads two elements.
        # Exercises store-then-load through the array root: a value hoisted
        # across the loop must not come from an element the loop rewrites.
        i = "i%d" % fresh(counter)
        name = "arr%d" % fresh(counter)
        n = rng.randint(2, 8)
        scope["arrays"].append((name, n))
        out.append("  double %s[%d];" % (name, n))
        out.append("  for (int %s = 0; %s < %d; ++%s) {" % (i, i, n, i))
        out.append("    %s[%s] = %s;" % (name, i, gen_expr(rng, scope, 2)))
        out.append("  }")
        j = rng.randint(0, n - 1)
        k = rng.randint(0, n - 1)
        e = "%s[%d] + %s[%d]" % (name, j, name, k)
        scope["exprs"].append(e)
        out.append("  double e%d = %s;" % (fresh(counter), e))
        scope["vars"].append("e%d" % counter[0])

    elif kind == "updinc":
        # `for (...; ++g)` -- the update slot advances something that is NOT the
        # loop counter. The counter is declared outside and stepped in the body
        # so the loop terminates (the first draft of effect_for_update.cpp got
        # this wrong and simply never ran). `++g` is a real effect: the effect
        # oracle used to skip the whole update slot (6ccef31).
        g = rng.choice(scope["vars"])
        i = "i%d" % fresh(counter)
        n = rng.randint(1, 3)
        out.append("  int %s = 0;" % i)
        out.append("  for (; %s < %d; ++%s) {" % (i, n, g))
        gen_body(rng, copy_scope(scope), depth - 1, out, counter)
        out.append("    ++%s;" % i)
        out.append("  }")


def gen_body(rng, scope, depth, out, counter):
    # Operates on the given scope: a declaration here is visible to later
    # statements in the same scope (and, at the top level, to the return).
    for _ in range(rng.randint(1, 4)):
        gen_stmt(rng, scope, depth, out, counter)


def gen_function(rng, idx):
    counter = [0]
    scope = {"vars": ["a", "b", "c"], "exprs": [], "arrays": []}
    body = []
    # Half the functions declare a local array up front, which is what makes the
    # element-store family (and its store-to-load hazards) reachable at all.
    # The array is never returned (the twin harness compares scalars), so every
    # function still ends with a scalar return.
    if rng.random() < 0.5:
        n = rng.choice([4, 8])
        name = "arr%d" % fresh(counter)
        scope["arrays"].append((name, n))
        body.append("  double %s[%d];" % (name, n))
    gen_body(rng, scope, 2, body, counter)
    ret = " + ".join(scope["vars"])
    lines = []
    lines.append("//@cse")
    lines.append("double sf_%d(double a, double b, double c) {" % idx)
    lines.extend(body)
    lines.append("  return %s;" % ret)
    lines.append("}")
    lines.append("double ref_%d(double a, double b, double c) {" % idx)
    lines.extend(body)
    lines.append("  return %s;" % ret)
    lines.append("}")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: gen_shapes.py <seed> <functions> <out.cpp>\n")
        return 2
    seed = int(sys.argv[1])
    count = int(sys.argv[2])
    out_path = sys.argv[3]
    rng = random.Random(seed)
    chunks = [gen_function(rng, i) for i in range(count)]
    with open(out_path, "w") as f:
        f.write("// Auto-generated by tests/verify/gen_shapes.py -- do not edit.\n")
        f.write("\n\n".join(chunks) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
