#!/usr/bin/env python3
"""Assert that generated output contains no self-assignment (`x = x;`).

CleanupPass removes self-assignments, including the vector-lowering artifacts
(`u_value[i] = u_value[i];`) that appear after LoopUnroll. A regression in that
removal is value-neutral and therefore invisible to the numeric verifiers, so
this scans the emitted text directly. Invoked from the csegen smoke block in
run_tests.sh.

A "self-assignment" is a simple lvalue -- a name followed by any chain of
subscripts / member accesses -- assigned from a textually identical lvalue. The
match runs on comment-stripped text so a comment that quotes the shape (as the
fixtures do) is not reported.

Usage: check_self_assign.py FILE [FILE ...]   (exit 1 if any match)
"""
import re
import sys

_LVAL = (r'[A-Za-z_]\w*'
         r'(?:\s*(?:\[[^\]\n]*\]|\.\s*[A-Za-z_]\w*|->\s*[A-Za-z_]\w*))*')
_SELF_ASSIGN = re.compile(r'(?<![\w.>])(' + _LVAL + r')\s*=\s*\1\s*;')


def _strip_comments(text):
    # Blank out comments rather than deleting them, so reported line numbers
    # still match the original file.
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text,
                  flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def main(argv):
    files = argv[1:]
    if not files:
        print('usage: check_self_assign.py FILE [FILE ...]', file=sys.stderr)
        return 2
    found = 0
    for path in files:
        try:
            with open(path, 'r', encoding='utf-8', errors='replace') as fh:
                src = _strip_comments(fh.read())
        except OSError as err:
            print(f'cannot read {path}: {err}', file=sys.stderr)
            return 2
        for lineno, line in enumerate(src.splitlines(), 1):
            for m in _SELF_ASSIGN.finditer(line):
                lhs = re.sub(r'\s+', '', m.group(1))
                print(f'{path}:{lineno}: self-assignment `{lhs} = {lhs};`',
                      file=sys.stderr)
                found += 1
    if found:
        print(f'{found} self-assignment(s) found', file=sys.stderr)
        return 1
    print(f'no self-assignments in {len(files)} file(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
