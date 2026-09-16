#!/usr/bin/env python3
"""Count pts-test cases (ptr -> target edges) in each dump file.

A dump is the `pts` section: the first line is the header `pts`, every other
non-empty line is `key;target;target...` (a record), and each target segment
is one test case.

Usage: dump_case_count.py [dumpDir] [outCsv]
Writes CSV columns: dump,cases
"""

import csv
import pathlib
import sys


HEADERS = {"pts", "basicBlock", "callGraph"}


def count_cases(path):
    n = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line in HEADERS:
            continue
        segs = line.split(";")
        n += max(0, len(segs) - 1)
    return n


def main():
    root = pathlib.Path(
        sys.argv[1] if len(sys.argv) > 1 else "report/exp260915/source"
    )
    out = pathlib.Path(
        sys.argv[2]
        if len(sys.argv) > 2
        else "report/exp260915/result/dumpCaseCount.csv"
    )

    dumps = sorted(root.rglob("*.pts")) if root.is_dir() else [root]
    rows = [
        (str(p.relative_to(root)) if root.is_dir() else p.name, count_cases(p))
        for p in dumps
    ]

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["dump", "cases"])
        writer.writerows(rows)

    total = sum(c for _, c in rows)
    print(f"{len(rows)} dumps, {total} cases -> {out}")


if __name__ == "__main__":
    main()
