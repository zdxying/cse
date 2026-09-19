#!/usr/bin/env python3
"""Guard against drift between the engine's hardcoded lattice table and
FreeLB's canonical src/lbm/lattice_set.h.

The engine mirrors direction vectors and weights in
plugins/freelb/lattice_resolve.cpp (`kDxQy{c,w}` arrays). FreeLB is the source
of truth (`latsetdata::c<D,Q>` / `w<D,Q>` specializations). This script parses
both and compares them.

Usage: check_lattice.py [--engine DIR] --freelb FREELB_DIR
"""
import argparse
import os
import re
import sys

LATTICES = ["D2Q5", "D2Q9", "D3Q7", "D3Q15", "D3Q19", "D3Q27"]
DIM = {"D2Q5": 2, "D2Q9": 2, "D3Q7": 3, "D3Q15": 3, "D3Q19": 3, "D3Q27": 3}
Q = {"D2Q5": 5, "D2Q9": 9, "D3Q7": 7, "D3Q15": 15, "D3Q19": 19, "D3Q27": 27}


def parse_freelb(path):
    text = open(path).read()
    out = {}
    for name in LATTICES:
        d, q = DIM[name], Q[name]
        c = re.search(
            r"c\s*<\s*%d\s*,\s*%d\s*>\s*\[%d\]\s*=\s*\{(.*?)\}\s*;" % (d, q, q),
            text, re.S)
        w = re.search(
            r"w\s*<\s*%d\s*,\s*%d\s*>\s*\[%d\]\s*=\s*\{(.*?)\}\s*;" % (d, q, q),
            text, re.S)
        if not c or not w:
            raise SystemExit("FreeLB: could not find c/w for %s" % name)
        dirs = [[int(v) for v in m.split(",")]
                for m in re.findall(r"\{([^{}]*)\}", c.group(1))]
        weights = [(int(n), int(dn)) for n, dn in
                   re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", w.group(1))]
        out[name] = (dirs, weights)
    return out


def parse_engine(path):
    text = open(path).read()
    out = {}
    for name in LATTICES:
        d, q = DIM[name], Q[name]
        c = re.search(r"k%s\s*c\[\]\s*=\s*\{(.*?)\}\s*;" % name, text, re.S)
        w = re.search(r"k%s\s*w\[\]\s*=\s*\{(.*?)\}\s*;" % name, text, re.S)
        if not c or not w:
            raise SystemExit("engine: could not find k%s{c,w}" % name)
        flat = [int(v) for v in re.findall(r"-?\d+", c.group(1))]
        if len(flat) != q * d:
            raise SystemExit("engine: k%sc has %d entries, want %d"
                             % (name, len(flat), q * d))
        dirs = [flat[i * d:(i + 1) * d] for i in range(q)]
        weights = []
        for n, dn in re.findall(r"(\d+(?:\.\d+)?)\s*/\s*(\d+(?:\.\d+)?)",
                                w.group(1)):
            weights.append((float(n), float(dn)))
        if len(weights) != q:
            raise SystemExit("engine: k%sw has %d entries, want %d"
                             % (name, len(weights), q))
        out[name] = (dirs, weights)
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=root)
    ap.add_argument("--freelb", required=True)
    args = ap.parse_args()

    hdr = os.path.join(args.freelb, "src", "lbm", "lattice_set.h")
    cpp = os.path.join(args.engine, "plugins", "freelb", "lattice_resolve.cpp")
    if not os.path.isfile(hdr):
        raise SystemExit("missing %s" % hdr)
    if not os.path.isfile(cpp):
        raise SystemExit("missing %s" % cpp)

    fb = parse_freelb(hdr)
    eng = parse_engine(cpp)

    failed = False
    for name in LATTICES:
        fdirs, fw = fb[name]
        edirs, ew = eng[name]
        problems = []
        if fdirs != edirs:
            problems.append("directions differ:\n  freelb=%s\n  engine=%s"
                            % (fdirs, edirs))
        for i, ((fn, fd), (en, ed)) in enumerate(zip(fw, ew)):
            if abs(fn / fd - en / ed) > 1e-15:
                problems.append("w[%d]: freelb=%d/%d engine=%g/%g"
                                % (i, fn, fd, en, ed))
        if problems:
            failed = True
            print("FAIL  %s" % name)
            for p in problems:
                print("      " + p)
        else:
            print("ok    %s" % name)

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
