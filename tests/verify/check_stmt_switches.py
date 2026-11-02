#!/usr/bin/env python3
"""Statement-kind switch shape check.

Every `switch (stmt->kind)` (and its local-variable spellings) over StmtIRKind
in src/ and plugins/ must either list every StmtIRKind explicitly (no
`default:`) or carry a `// partial:` marker on the `default:` line -- a partial
switch is a deliberate choice to handle only some statement kinds, and the
marker forces that choice to be written down. A bare `default:` would silently
swallow a new StmtIRKind instead of making the omission a compile error, which
is the exact failure mode the exhaustive-switch convention exists to prevent.

Node-kind switches (DAGNode) are deliberately out of scope: many of them
classify only a subset of node kinds by design (purity questions, cost
weights), and -Wswitch-enum noise there is exactly why this is a targeted
shape check rather than a compiler flag.

Exit 0 iff no unmarked `default:` exists in any statement-kind switch.
"""

import glob
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
# Statement-kind switches look like `switch (stmt->kind)`, `switch
# (s->kind)`, `switch (f->body->kind)` -- any `x->kind` on an expression
# spelled in statement terms. Node switches say `node->kind` / `n->kind` /
# `e->kind`, so anchor on the statement spellings actually used.
SWITCH_RE = re.compile(r"switch \(((?:const )?\w*stmt\w*|[sfblb]|body)(->|\.get\(\)->)kind\)")
DEFAULT_RE = re.compile(r"^(\s*)default:")

files = sorted(
    set(
        glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True)
        + glob.glob(os.path.join(ROOT, "src", "**", "*.h"), recursive=True)
        + glob.glob(os.path.join(ROOT, "plugins", "**", "*.cpp"), recursive=True)
        + glob.glob(os.path.join(ROOT, "plugins", "**", "*.h"), recursive=True)
    )
)

violations = []
switches = 0
for f in files:
    lines = open(f).read().splitlines()
    for i, line in enumerate(lines):
        if not SWITCH_RE.search(line):
            continue
        switches += 1
        depth = 0
        started = False
        for j in range(i, min(i + 200, len(lines))):
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                started = True
            if started and depth <= 0:
                break
            m = DEFAULT_RE.match(lines[j])
            if m:
                nxt = lines[j + 1] if j + 1 < len(lines) else ""
                if "partial:" not in lines[j] and "partial:" not in nxt:
                    violations.append(f"{f}:{j + 1}")

print(f"statement-kind switches: {switches}")
for v in violations:
    print(f"FAIL  unmarked default: in a statement-kind switch: {v}")
if violations:
    sys.exit(1)
print("ok    every statement-kind switch is exhaustive or marked partial:")
