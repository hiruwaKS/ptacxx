#!/usr/bin/env python3
"""
Usage:
    python3 sourceReduce.py [options] <src-path> <out-path> <feasible-cmd>

Reduce the source file to the smallest (fewest lines) version that still
satisfies the feasibility condition given by <feasible-cmd>.

This is a thin wrapper over agentSearch.optimize: it calls the opencode agent
search with the feasibility command as the constraint and the file line count
as the optimization target.

Positional Arguments:
    <src-path>       Input file/directory path (source file path). This is also
                     the working file that candidate versions are written to
                     before feasible-cmd runs.
    <out-path>       Output file/directory path for the best (smallest) result
                     found (can be the same as src-path).
    <feasible-cmd>   Feasibility command: succeeds only for a valid candidate,
                     e.g. a compile command. A candidate must make this command
                     exit 0 to be kept. (must be wrapped in quotes)

Options:
    --config <PATH>    Path to an opencode config file (required), e.g.
                       build/opencode.json. See agentSearch.py.
    --model <ID>       OpenCode model reference to use. Default: the first
                       model in the config's deepseek provider.
    --target-cmd <CMD> Objective command to minimize. Default: count lines of
                       the working file:
                         "wc -l < <src-path>"
    --auto             Pass --auto to opencode run (default: on).
    --no-auto          Do not auto-approve permission prompts.
    --max-iters <N>    Maximum total iterations. Default: 20.
    --stall <N>        Stop after N consecutive iterations with no line-count
                       improvement. Default: 3.
    --timeout <SEC>    Overall run timeout in seconds. Default: 300.
    --hint <TEXT>      Initial guidance telling the agent what direction to reduce
                       toward (e.g. remove unused struct fields / dead code).
    --hint-file <PATH> Read the guidance prompt from a file instead of the
                       command line. Useful for longer prompts.
    --help             Show this help message and exit.

Examples:
    python3 sourceReduce.py --config build/opencode.json a.c a.c "gcc -c a.c"
    python3 sourceReduce.py --config cfg.json --max-iters 30 a.py a.py "python3 a.py > out && diff out expected"
"""

import argparse
import sys

import agentSearch

RED = "\033[31m"
RESET = "\033[0m"


def main():
    parser = argparse.ArgumentParser(
        description="Reduce source code to the smallest feasible version.",
        usage="sourceReduce.py [options] <src-path> <out-path> <feasible-cmd>",
    )
    parser.add_argument("src_path", metavar="src-path")
    parser.add_argument("out_path", metavar="out-path")
    parser.add_argument("feasible_cmd", metavar="feasible-cmd")
    parser.add_argument("--config", required=True, help="Path to opencode config file")
    parser.add_argument("--model", default=None, help="Model reference (provider/model)")
    parser.add_argument("--target-cmd", default=None, help="Objective command to minimize")
    parser.add_argument("--auto", dest="auto", action="store_true", default=True)
    parser.add_argument("--no-auto", dest="auto", action="store_false")
    parser.add_argument("--max-iters", type=int, default=20, help="Default: 20")
    parser.add_argument("--stall", type=int, default=3, help="Default: 3")
    parser.add_argument("--timeout", type=float, default=300, help="Default: 300")
    parser.add_argument(
        "--hint", default=None,
        help="Initial guidance telling the agent what direction to reduce toward",
    )
    parser.add_argument(
        "--hint-file", default=None, metavar="PATH",
        help="Read the initial guidance from a file (you write the prompt here)",
    )
    parser.add_argument(
        "--variant", default=None,
        help="Model reasoning effort (e.g. minimal, low, high)",
    )
    args = parser.parse_args()

    target_cmd = args.target_cmd or f"wc -l < {args.src_path}"

    hint = args.hint
    if args.hint_file:
        with open(args.hint_file, "r", encoding="utf-8") as f:
            hint = f.read()

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
            hint=hint,
            variant=args.variant,
        )
    except (ValueError, RuntimeError) as exc:
        sys.stderr.write(f"{RED}sourceReduce.py: {exc}{RESET}\n")
        sys.stderr.flush()
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
