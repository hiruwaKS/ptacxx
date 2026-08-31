#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import time


TIMEOUT_SECONDS = 120


def read_paths(path_file):
    paths = []
    for line in path_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        paths.append(pathlib.Path(line))
    return paths


def collect_bitcodes(paths):
    bitcodes = []
    for path in paths:
        if path.is_dir():
            bitcodes.extend(path.glob("*.bc"))
        elif path.suffix in (".bc", ".ll"):
            bitcodes.append(path)
    return sorted(bitcodes)


def query_lines(stdout):
    lines = []
    inside = False
    for line in stdout.splitlines():
        if line.strip() == "<queryresult>":
            inside = True
            continue
        if line.strip() == "</queryresult>":
            inside = False
            continue
        if inside and line.strip() and not line.strip().startswith("summary"):
            lines.append(line.strip())
    return lines


def main():
    if len(sys.argv) < 3:
        print(
            "usage: runTest.py <executable> <testFilePath.txt> [--testTimeOut seconds] [args...]",
            file=sys.stderr,
        )
        return 2

    executable = pathlib.Path(sys.argv[1])
    path_file = pathlib.Path(sys.argv[2])
    timeout_seconds = TIMEOUT_SECONDS
    analyzer_args = []
    idx = 3
    while idx < len(sys.argv):
        if sys.argv[idx] == "--testTimeOut":
            if idx + 1 >= len(sys.argv):
                print("--testTimeOut requires seconds", file=sys.stderr)
                return 2
            timeout_seconds = int(sys.argv[idx + 1])
            idx += 2
            continue
        analyzer_args.append(sys.argv[idx])
        idx += 1
    bitcodes = collect_bitcodes(read_paths(path_file))
    failures = []
    errors = []
    start = time.time()

    for idx, bitcode in enumerate(bitcodes, 1):
        cmd = [str(executable), str(bitcode), *analyzer_args]
        try:
            proc = subprocess.run(
                cmd,
                input="test\n",
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout_seconds,
            )
        except subprocess.TimeoutExpired:
            errors.append((str(bitcode), "TIMEOUT", ""))
            print(f"[{idx}/{len(bitcodes)}] TIMEOUT {bitcode.name}", flush=True)
            continue

        output = proc.stdout + "\n" + proc.stderr
        summaries = [
            line.strip()
            for line in output.splitlines()
            if line.strip().startswith("summary total=")
        ]
        summary = summaries[-1] if summaries else ""
        fail_match = re.search(r"\bFail=(\d+)\b", summary)
        fail_count = int(fail_match.group(1)) if fail_match else None

        if proc.returncode != 0 or fail_count is None:
            errors.append((str(bitcode), f"rc={proc.returncode}", summary or output[-1200:]))
            print(
                f"[{idx}/{len(bitcodes)}] ERROR {bitcode.name} "
                f"rc={proc.returncode} summary={summary!r}",
                flush=True,
            )
        elif fail_count != 0:
            failures.append((str(bitcode), summary, query_lines(proc.stdout)))
            print(f"[{idx}/{len(bitcodes)}] FAIL {bitcode.name} {summary}", flush=True)
        else:
            print(f"[{idx}/{len(bitcodes)}] OK {bitcode.name} {summary}", flush=True)

    print("===RESULT===")
    print(
        f"total={len(bitcodes)} failures={len(failures)} "
        f"errors={len(errors)} elapsed={time.time() - start:.1f}s"
    )

    if failures:
        print("===FAILURES===")
        for bitcode, summary, lines in failures:
            print(bitcode)
            print(summary)
            for line in lines:
                print(line)

    if errors:
        print("===ERRORS===")
        for bitcode, error, detail in errors:
            print(bitcode)
            print(error)
            print(detail)

    return 1 if failures or errors else 0


if __name__ == "__main__":
    sys.exit(main())
