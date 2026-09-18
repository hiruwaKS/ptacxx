#!/usr/bin/env python3

import csv
import pathlib
import re
import signal
import sys
import time

from parallel import ParallelHelper, TaskResult, run_process


TIMEOUT_SECONDS = 120
DUMP_SUFFIX = ".inst.dump.txt.pts"
CRASH_EXCERPT_LIMIT = 4000
DEFAULT_CSV = "runTestForDump.csv"

CSV_FIELDS = [
    "dump",
    "status",
    "fails",
    "total",
    "pass",
    "rate",
    "error",
    "result",
    "detail",
]

ERROR_LABELS = {
    "INIT_SEGFAULT": "init_segfault",
    "QUERY_SEGFAULT": "query_segfault",
    "INIT_ABORT": "init_abort",
    "QUERY_ABORT": "query_abort",
    "INIT_ERROR": "initerror",
    "QUERY_ERROR": "queryerror",
    "INIT_OTHER": "init_other",
    "QUERY_OTHER": "query_other",
    "MISSING": "missing",
}

# Every ptacxx error carries a unique name (`<category>-kebab-detail`, see
# docs/ErrorUniqueName.csv) as the first token of what(); recover it so any
# error code is labeled generically instead of special-casing a few.
ERROR_NAME_RE = re.compile(r"^([a-z][a-z0-9]*(?:-[a-z0-9]+)+)\b")


def collect_dumps(root):
    if root.is_file():
        return [root] if root.suffix == ".pts" else []
    return sorted(root.rglob("*.pts"))


def module_for(dump):
    name = dump.name
    if name.endswith(DUMP_SUFFIX):
        return dump.with_name(name[: -len(DUMP_SUFFIX)] + ".ll")
    return dump.with_suffix(".ll")


def query_result(stdout):
    lines = []
    inside = False
    for line in stdout.splitlines():
        if line.strip() == "<queryresult>":
            inside = True
            continue
        if line.strip() == "</queryresult>":
            break
        if inside:
            lines.append(line)
    return lines


def describe_rc(rc):
    if rc is not None and rc < 0:
        try:
            return f"rc={rc} ({signal.Signals(-rc).name})"
        except ValueError:
            return f"rc={rc}"
    return f"rc={rc}"


def signal_name(rc):
    if rc is None or rc >= 0:
        return None
    try:
        return signal.Signals(-rc).name
    except ValueError:
        return None


def extract_tag(text, tag):
    """Return the body of a `<tag>...</tag>` block, or None if absent."""
    match = re.search(rf"<{tag}>\n(.*?)\n</{tag}>", text, re.S)
    return match.group(1).strip() if match else None


def error_name(text):
    """Extract the leading unique error name from an error body, if any."""
    match = ERROR_NAME_RE.match((text or "").strip())
    return match.group(1) if match else None


def classify_error(proc, lines, result):
    """Split a non-OK exit into init/query-phase segfault/abort/error/other.

    `<init>` is emitted only after init() returns, so its presence means the
    failure happened while serving a query, not during initialization. The
    init/query phase stays the status prefix; exceptions additionally carry
    their unique error name, so every code is reported without special-casing.
    """
    rc = proc.returncode

    init_error = extract_tag(proc.stdout, "initerror")
    if init_error is not None:
        name = error_name(init_error)
        if name:
            return f"INIT_{name}", name, init_error
        return "INIT_ERROR", "initerror", init_error

    query_error = extract_tag(proc.stdout, "queryerror")
    if query_error is not None:
        name = error_name(query_error)
        if name:
            return f"QUERY_{name}", name, query_error
        return "QUERY_ERROR", "queryerror", query_error

    phase = "QUERY" if extract_tag(proc.stdout, "init") is not None else "INIT"
    detail = crash_excerpt(proc.stderr + proc.stdout)

    name = signal_name(rc)
    if name == "SIGSEGV":
        return f"{phase}_SEGFAULT", describe_rc(rc), detail
    if name == "SIGABRT":
        return f"{phase}_ABORT", describe_rc(rc), detail

    if not lines:
        return f"{phase}_OTHER", f"{describe_rc(rc)} (no error tag)", detail
    return f"{phase}_OTHER", f"{describe_rc(rc)} (no error tag)", "\n".join(lines)


def crash_excerpt(text, limit=CRASH_EXCERPT_LIMIT):
    """Keep the head (assertion + top frames) and tail of a crash log."""
    text = text.strip()
    if len(text) <= limit:
        return text
    head = limit * 3 // 4
    tail = limit - head
    return f"{text[:head]}\n... [truncated] ...\n{text[-tail:]}"


def csv_row(dump, status, fails="", total="", error="", result="", detail=""):
    """Build one CSV record, deriving pass/rate from fails/total when known."""
    if isinstance(fails, int) and isinstance(total, int):
        passed = total - fails
        rate = round(100.0 * fails / total, 2) if total else 0.0
    else:
        passed = ""
        rate = ""
    return {
        "dump": str(dump),
        "status": status,
        "fails": fails,
        "total": total,
        "pass": passed,
        "rate": rate,
        "error": error,
        "result": result,
        "detail": detail,
    }


def write_csv(path, rows):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def run_one(dump, module, executable, analyzer_args, timeout, consistent=False):
    """Run one dump and return a TaskResult with a parsed payload."""
    query = f"pts-test {dump}" + (" -c" if consistent else "")
    proc = run_process(
        [str(executable), str(module), *analyzer_args],
        timeout,
        stdin=f"{query}\n",
    )
    if proc.timed_out:
        return TaskResult("TIMEOUT", dump)

    lines = query_result(proc.stdout)
    result = lines[0] if lines else ""
    fail_match = re.search(r"\bFail=(\d+)/(\d+)\b", result)
    if proc.returncode != 0 or not fail_match:
        status, error, detail = classify_error(proc, lines, result)
        return TaskResult(
            status,
            dump,
            {
                "error": error,
                "result": result,
                "detail": detail,
            },
        )

    fails = int(fail_match.group(1))
    total = int(fail_match.group(2))
    return TaskResult(
        "FAIL" if fails else "OK",
        dump,
        {"fails": fails, "total": total, "result": result, "extra": lines[1:]},
    )


def main():
    if len(sys.argv) < 3:
        print(
            "usage: runTestForDump.py <executable> <dumpDir> "
            "[--testTimeOut seconds] [--jobs N] [--top N] [--csv path] "
            "[--consistent] [args...]",
            file=sys.stderr,
        )
        return 2

    executable = pathlib.Path(sys.argv[1])
    dump_root = pathlib.Path(sys.argv[2])
    timeout_seconds = TIMEOUT_SECONDS
    jobs = 0
    top_details = 0
    csv_path = pathlib.Path(DEFAULT_CSV)
    consistent = False
    analyzer_args = []
    idx = 3
    while idx < len(sys.argv):
        arg = sys.argv[idx]
        if arg == "--testTimeOut":
            if idx + 1 >= len(sys.argv):
                print("--testTimeOut requires seconds", file=sys.stderr)
                return 2
            timeout_seconds = int(sys.argv[idx + 1])
            idx += 2
            continue
        if arg == "--jobs":
            if idx + 1 >= len(sys.argv):
                print("--jobs requires a count", file=sys.stderr)
                return 2
            jobs = int(sys.argv[idx + 1])
            idx += 2
            continue
        if arg == "--top":
            if idx + 1 >= len(sys.argv):
                print("--top requires a count", file=sys.stderr)
                return 2
            top_details = int(sys.argv[idx + 1])
            idx += 2
            continue
        if arg == "--csv":
            if idx + 1 >= len(sys.argv):
                print("--csv requires a path", file=sys.stderr)
                return 2
            csv_path = pathlib.Path(sys.argv[idx + 1])
            idx += 2
            continue
        if arg == "--consistent":
            consistent = True
            idx += 1
            continue
        analyzer_args.append(arg)
        idx += 1

    dumps = collect_dumps(dump_root)
    if not dumps:
        print(f"no .pts dumps found under {dump_root}", file=sys.stderr)
        return 2

    records = []
    failures = []
    errors = []
    error_counts = {}
    rows = []
    finalized = 0
    total_dumps = len(dumps)

    def done_line(status, dump, result=""):
        nonlocal finalized
        finalized += 1
        suffix = f" {result}" if result else ""
        print(f"[{finalized}/{total_dumps}] {status} {dump.name}{suffix}", flush=True)

    runnable = []
    for dump in dumps:
        module = module_for(dump)
        if not module.is_file():
            errors.append((str(dump), "MISSING", f"module not found: {module}", ""))
            error_counts["MISSING"] = error_counts.get("MISSING", 0) + 1
            rows.append(
                csv_row(dump, "missing", error=f"module not found: {module}")
            )
            done_line("MISSING", dump, f"module not found: {module.name}")
        else:
            runnable.append((dump, module))

    def runner(item, timeout):
        dump, module = item
        return run_one(dump, module, executable, analyzer_args, timeout, consistent)

    def on_result(res):
        dump = res.item
        payload = res.payload or {}
        if res.status == "OK":
            records.append((str(dump), 0, payload["total"]))
            rows.append(
                csv_row(
                    dump,
                    "ok",
                    fails=0,
                    total=payload["total"],
                    result=payload["result"],
                )
            )
            done_line("OK", dump, payload["result"])
        elif res.status == "FAIL":
            records.append((str(dump), payload["fails"], payload["total"]))
            failures.append(
                (
                    str(dump),
                    payload["fails"],
                    payload["total"],
                    payload["result"],
                    payload["extra"],
                )
            )
            rows.append(
                csv_row(
                    dump,
                    "fail",
                    fails=payload["fails"],
                    total=payload["total"],
                    result=payload["result"],
                    detail="\n".join(payload["extra"]),
                )
            )
            done_line("FAIL", dump, payload["result"])
        elif res.status == "TIMEOUT":
            errors.append((str(dump), "TIMEOUT", "TIMEOUT", ""))
            error_counts["TIMEOUT"] = error_counts.get("TIMEOUT", 0) + 1
            rows.append(csv_row(dump, "timeout", error="TIMEOUT"))
            done_line("TIMEOUT", dump)
        else:
            errors.append((str(dump), res.status, payload["error"], payload["detail"]))
            error_counts[res.status] = error_counts.get(res.status, 0) + 1
            rows.append(
                csv_row(
                    dump,
                    ERROR_LABELS.get(res.status, res.status.lower()),
                    error=payload["error"],
                    result=payload.get("result", ""),
                    detail=payload["detail"],
                )
            )
            done_line(res.status, dump, payload["error"])

    helper = ParallelHelper(max_workers=jobs, timeout_steps=(timeout_seconds,))
    print(
        f"running {len(runnable)} dump(s) with "
        f"{helper.worker_count(len(runnable))} worker(s)",
        flush=True,
    )

    start = time.time()
    helper.run(runnable, runner, on_result)
    elapsed = time.time() - start

    tested_total = sum(total for _, _, total in records)
    tested_fails = sum(fails for _, fails, _ in records)
    tested_pass = tested_total - tested_fails
    overall_rate = 100.0 * tested_fails / tested_total if tested_total else 0.0

    print("===SUMMARY===")
    print(
        f"dumps  total={len(dumps)} ok={len(records) - len(failures)} "
        f"fail={len(failures)} error={len(errors)}"
    )
    breakdown = " ".join(
        f"{ERROR_LABELS.get(status, status.lower())}={count}"
        for status, count in sorted(error_counts.items())
    )
    print(f"errors {breakdown if breakdown else 'none'}")
    print(
        f"cases  total={tested_total} pass={tested_pass} "
        f"fail={tested_fails} rate={overall_rate:.2f}%"
    )
    print(f"time   elapsed={elapsed:.1f}s")

    try:
        write_csv(csv_path, rows)
    except OSError as exc:
        print(f"failed to write csv {csv_path}: {exc}", file=sys.stderr)
    else:
        print(f"csv    written={csv_path} rows={len(rows)}")

    ranked = sorted(failures, key=lambda r: (r[1], r[2]), reverse=True)

    if ranked:
        print(f"===FAILED DUMPS ({len(ranked)})===")
        print(f"{'fails':>7} {'total':>7} {'rate':>8}  dump")
        for dump, fails, total, _result, _extra in ranked:
            rate = 100.0 * fails / total if total else 0.0
            print(f"{fails:>7} {total:>7} {rate:>7.2f}%  {dump}")

    if errors:
        print(f"===ERRORS ({len(errors)})===")
        for dump, status, error, detail in errors:
            print(f"{dump} [{ERROR_LABELS.get(status, status.lower())}]")
            print(f"  {error}")
            for line in detail.splitlines():
                print(f"  {line}")

    shown = ranked if top_details <= 0 else ranked[:top_details]
    if shown:
        print(f"===FAIL DETAILS (showing {len(shown)}/{len(ranked)})===")
        for dump, fails, total, result, extra in shown:
            print(dump)
            print(f"  {result}")
            for line in extra:
                print(f"  {line}")

    return 1 if failures or errors else 0


if __name__ == "__main__":
    sys.exit(main())
