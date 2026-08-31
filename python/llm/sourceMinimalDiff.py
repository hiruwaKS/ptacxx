#!/usr/bin/env python3
"""
Usage:
    python3 sourceMinimalDiff.py [options] <src-path> <out-path> <ref-path> <feasible-cmd>

Under the feasibility constraint <feasible-cmd>, reduce <src-path> so that its
diff against <ref-path> is as small as possible.

This is a thin wrapper over agentSearch.optimize: the feasibility command is
the constraint, and the objective is the size of `diff <src> <ref>` (fewer
bytes / lines of diff = more similar).

Positional Arguments:
    <src-path>       Input file path (source to optimize). Also the working
                     file that candidate versions are written to before
                     feasible-cmd runs.
    <out-path>       Output file path for the best result (can be the same as
                     src-path).
    <ref-path>       Reference file that the optimized source is compared
                     against with diff.
    <feasible-cmd>   Feasibility command: succeeds only for a valid candidate,
                     e.g. a compile command. Must exit 0 for a candidate to be
                     kept. (must be wrapped in quotes)

Options:
    --config <PATH>    Path to an opencode config file (required), e.g.
                       build/opencode.json. See agentSearch.py.
    --model <ID>       OpenCode model reference to use. Default: the first
                       model in the config's deepseek provider.
    --target-cmd <CMD> Objective command to minimize. Default: diff size
                       against the reference:
                         "diff <src-path> <ref-path> | wc -c"
    --auto             Pass --auto to opencode run (default: on).
    --no-auto          Do not auto-approve permission prompts.
    --max-iters <N>    Maximum total iterations. Default: 20.
    --stall <N>        Stop after N consecutive iterations with no diff-size
                       improvement. Default: 3.
    --timeout <SEC>    Overall run timeout in seconds. Default: 300.
    --help             Show this help message and exit.

Examples:
    python3 sourceMinimalDiff.py --config build/opencode.json mine.c out.c ref.c "gcc -c mine.c"
    python3 sourceMinimalDiff.py --config cfg.json a.py out.py ref.py "python3 a.py > o && diff o expected"
"""

import argparse
import sys

import agentSearch

RED = "\033[31m"
RESET = "\033[0m"


def main():
    parser = argparse.ArgumentParser(
        description="Reduce source code to minimize its diff against a reference.",
        usage="sourceMinimalDiff.py [options] <src-path> <out-path> <ref-path> <feasible-cmd>",
    )
    parser.add_argument("src_path", metavar="src-path")
    parser.add_argument("out_path", metavar="out-path")
    parser.add_argument("ref_path", metavar="ref-path")
    parser.add_argument("feasible_cmd", metavar="feasible-cmd")
    parser.add_argument("--config", required=True, help="Path to opencode config file")
    parser.add_argument("--model", default=None, help="Model reference (provider/model)")
    parser.add_argument("--target-cmd", default=None, help="Objective command to minimize")
    parser.add_argument("--auto", dest="auto", action="store_true", default=True)
    parser.add_argument("--no-auto", dest="auto", action="store_false")
    parser.add_argument("--max-iters", type=int, default=20, help="Default: 20")
    parser.add_argument("--stall", type=int, default=3, help="Default: 3")
    parser.add_argument("--timeout", type=float, default=300, help="Default: 300")
    args = parser.parse_args()

    target_cmd = args.target_cmd or f"diff {args.src_path} {args.ref_path} | wc -c"

    try:
        result = agentSearch.optimize(
            src_path=args.src_path,
            out_path=args.out_path,
            feasible_cmd=args.feasible_cmd,
            target_cmd=target_cmd,
            config_path=args.config,
            model=args.model,
            auto=args.auto,
            max_iters=args.max_iters,
            stall=args.stall,
            timeout=args.timeout,
        )
    except (ValueError, RuntimeError) as exc:
        sys.stderr.write(f"{RED}sourceMinimalDiff.py: {exc}{RESET}\n")
        sys.stderr.flush()
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
