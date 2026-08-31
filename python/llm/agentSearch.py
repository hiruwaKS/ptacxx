#!/usr/bin/env python3
"""
Usage:
    python3 agentSearch.py [options] <src-path> <out-path> <feasible-cmd> <target-cmd>

Positional Arguments:
    <src-path>       Input file/directory path (source file path). This is also
                     the working file that candidate versions are written to
                     before feasible-cmd / target-cmd run.
    <out-path>       Output file/directory path for the best result found
                     (can be the same as src-path).
    <feasible-cmd>   Feasibility command: verifies that a candidate preserves
                     the required behavior. A candidate is accepted only if
                     this command succeeds (exit code 0).
                     (must be wrapped in quotes)
    <target-cmd>     OPTIONAL optimization objective command: produces a numeric
                     output (e.g. count lines) that is minimized. If omitted,
                     the LLM judges improvement on its own. When given, its
                     value is shown to the LLM as a hint, but the LLM still
                     makes the final accept/reject decision.
                     (must be wrapped in quotes)

Options:
    --config <PATH>    Path to an opencode config file (required), e.g. the
                       build/opencode.json with a provider + auth block.
                       Used via the OPENCODE_CONFIG env var when running the
                       opencode CLI. The DeepSeek model id is read from it.
    --model <ID>       OpenCode model reference to use, e.g.
                       deepseek/deepseek-v4-flash. Default: the first model
                       declared in the config's deepseek provider.
    --auto             Pass --auto to opencode run (auto-approve permission
                       prompts). Default: on.
    --max-iters <N>    Maximum total iterations before stopping.
                       Default: 20.
    --stall <N>        Stop after N consecutive iterations with no
                       improvement in the target value. Default: 3.
    --timeout <SEC>    Overall run timeout in seconds. Default: 300.
    --help             Show this help message and exit.

Optimization problem:
    subject to feasible-cmd(candidate) succeeds
    minimize the objective (numeric target-cmd, if given), judged by the LLM

    Each iteration runs the opencode CLI (configured by --config) with the
    current best source, asking it to produce a candidate that is feasible.
    A candidate replaces the current best only if:
      1. feasible-cmd succeeds, AND
      2. the LLM decides the candidate is an improvement (it may weigh the
         numeric target-cmd output, or apply its own judgment).

    Stop conditions (whichever comes first):
      1. --stall consecutive iterations with no target improvement
      2. total iterations reach --max-iters
      3. --timeout seconds elapse

Examples:
    python3 agentSearch.py --config build/opencode.json a.cpp a.cpp "gcc a.cpp -o t && ./t > out && diff out expected" "wc -c < a.cpp"
    python3 agentSearch.py --config cfg.json --max-iters 30 ./src ./out "python3 build.py" "wc -l < candidate.cpp"
    python3 agentSearch.py --config cfg.json ./src ./out "python3 build.py"   # no target: LLM judges improvement
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

RED = "\033[31m"
RESET = "\033[0m"


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def log_red(msg):
    log(f"{RED}{msg}{RESET}")


def read_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def write_file(path, content):
    with open(path, "w", encoding="utf-8", errors="replace") as f:
        f.write(content)


def first_model_id(config):
    """Pick the first model id from the deepseek provider in the config."""
    try:
        models = config["provider"]["deepseek"]["models"]
    except (KeyError, TypeError):
        return None
    if not isinstance(models, dict):
        return None
    for mid in models:
        return f"deepseek/{mid}"
    return None


def query_llm(config_path, model, prompt, timeout, workdir, auto=True, variant=None):
    """Run `opencode run --format json` and return the assistant's text."""
    env = dict(os.environ)
    env["OPENCODE_CONFIG"] = config_path
    cmd = [
        "opencode",
        "run",
        "--format", "json",
        "--model", model,
        "--dir", workdir,
    ]
    if variant:
        cmd += ["--variant", variant]
    if auto:
        cmd.append("--auto")
    cmd.append(prompt)

    try:
        proc = subprocess.run(
            cmd, env=env, text=True, capture_output=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return None, "opencode timed out"

    if proc.returncode != 0:
        return None, (proc.stderr or proc.stdout).strip()

    parts = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("type") == "text":
            part = event.get("part", {})
            if part.get("type") == "text":
                parts.append(part.get("text", ""))
    return "".join(parts), ""


def extract_candidate(output):
    """Strip markdown code fences / surrounding noise to get the raw source."""
    if not output:
        return ""
    text = output.strip("\n").strip()
    m = re.search(r"```(?:[a-zA-Z0-9_+-]+)?\n(.*?)\n```", text, re.DOTALL)
    if m:
        return m.group(1)
    return text


def run_command(cmd, timeout):
    """Run a shell command; return (returncode, stdout, stderr)."""
    try:
        proc = subprocess.run(
            cmd, shell=True, text=True, capture_output=True, timeout=timeout
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def parse_target(output):
    """Extract the first float from the target command output."""
    if not output:
        return None
    m = re.search(r"[-+]?\d+(?:\.\d+)?", output)
    if not m:
        return None
    try:
        return float(m.group(0))
    except ValueError:
        return None


def load_config(config_path):
    """Load the opencode JSON config file."""
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            config = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load config '{config_path}': {exc}") from exc
    if not isinstance(config, dict):
        raise ValueError("config must be a JSON object")
    return config


def resolve_model(config, model):
    """Return the model reference, defaulting to the first deepseek model."""
    return model or first_model_id(config)


def optimize(
    src_path,
    out_path,
    feasible_cmd,
    target_cmd=None,
    config_path=None,
    model=None,
    auto=True,
    max_iters=20,
    stall=3,
    timeout=300,
    hint=None,
    variant=None,
):
    """Optimize src so that feasible_cmd succeeds and the objective improves.

    When target_cmd is given, its numeric output is reported to the LLM as a hint,
    but the LLM makes the final accept/reject decision. When target_cmd is omitted,
    the LLM judges quality purely on its own. Writes the best result to out_path and
    returns a dict with the best content and the achieved target value.

    Can be used as a library (import agentSearch; agentSearch.optimize(...))
    or via the CLI.
    """
    if max_iters < 1:
        raise ValueError("--max-iters must be >= 1")
    if stall < 1:
        raise ValueError("--stall must be >= 1")

    config = load_config(config_path)
    model = resolve_model(config, model)
    if not model:
        raise ValueError("cannot determine model from config; use --model")

    workdir = os.path.dirname(os.path.abspath(src_path)) or "."
    best = read_file(src_path)
    best_value = None
    stall_limit = stall

    def evaluate(content):
        """Write candidate to the working file; return (feasible, value)."""
        write_file(src_path, content)
        rc, _, ferr = run_command(feasible_cmd, 30)
        if rc != 0:
            return False, f"feasible failed (rc={rc}): {ferr.strip()}"
        if not target_cmd:
            return True, ""
        _, tout, terr = run_command(target_cmd, 30)
        value = parse_target(tout)
        if value is None:
            return True, "warning: cannot parse target value"
        return True, value

    feasible, res = evaluate(best)
    if not feasible:
        write_file(src_path, best)
        raise RuntimeError(f"baseline is infeasible: {res}")
    if target_cmd:
        best_value = res
    log(f"baseline target value: {best_value if target_cmd else 'n/a (LLM-judged)'}")
    log(f"using opencode model: {model}")

    start = time.time()

    def judge(candidate, cand_value, best_content, best_value):
        """Ask the LLM whether the candidate is a genuine improvement."""
        hint = ""
        if target_cmd:
            hint = (
                "\nNumeric objective (lower is better):\n"
                f"  current best = {best_value}\n"
                f"  candidate    = {cand_value}\n"
            )
        prompt = (
            "You decide whether a candidate is a genuine improvement over the "
            "current best. Both versions pass the feasibility check.\n"
            "Current best:\n----------\n"
            f"{best_content}\n"
            "----------\n"
            "Candidate:\n----------\n"
            f"{candidate}\n"
            "----------\n"
            f"{hint}"
            "Treat the numeric objective as a hint if given, but apply your own "
            "judgment (correctness, robustness, clarity, size, etc.).\n"
            "IMPORTANT: Do NOT run any commands and do NOT call any tools. Just emit text.\n"
            "Reply with exactly one token: YES (candidate is better, keep it) or NO.\n"
        )
        remaining = max(1.0, timeout - (time.time() - start))
        call_timeout = min(120.0, remaining)
        output, err = query_llm(config_path, model, prompt, call_timeout, workdir, auto, variant)
        if err:
            log(f"  judge: opencode failed: {err}")
            return False
        return (output or "").strip().upper().startswith("YES")

    stall_count = 0
    iters = 0
    t_llm = t_eval = t_judge = 0.0
    feedback = []
    while iters < max_iters and time.time() - start < timeout:
        iters += 1
        feedback_text = ""
        if feedback:
            feedback_text = (
                "\nFeedback from your previous attempts (each is your output, then its result):\n"
                + "\n".join(feedback[-5:])
                + "\n"
            )
        prompt = (
            (f"Guidance / objective direction: {hint}\n" if hint else "")
            + feedback_text
            + "Current content:\n"
            "----------\n"
            f"{best}\n"
            "----------\n"
        )
        remaining = max(1.0, timeout - (time.time() - start))
        call_timeout = min(300.0, remaining)
        log(f"iteration {iters}: querying opencode ({model})...")
        t0 = time.time()
        output, err = query_llm(config_path, model, prompt, call_timeout, workdir, auto, variant)
        t1 = time.time()
        t_llm += t1 - t0
        log(f"iteration {iters}:   [llm]  {t1 - t0:6.1f}s")
        if err:
            log_red(f"iteration {iters}: opencode failed: {err}")
            break

        candidate = extract_candidate(output)
        if not candidate.strip():
            log_red(f"iteration {iters}: empty candidate from LLM")
            stall_count += 1
            if stall_count >= stall_limit:
                log("stop: too many stalled iterations")
                break
            continue

        t2 = time.time()
        feasible, res = evaluate(candidate)
        t3 = time.time()
        t_eval += t3 - t2
        log(f"iteration {iters}:   [eval] {t3 - t2:6.1f}s")
        if not feasible:
            log(f"iteration {iters}: candidate rejected ({res})")
            feedback.append(f"  round {iters}: candidate {len(candidate)} chars -> REJECTED ({res})")
            write_file(src_path, best)
            stall_count += 1
        else:
            cand_value = res if target_cmd else None
            improved = judge(candidate, cand_value, best, best_value)
            t_judge += time.time() - t3
            if improved:
                best = candidate
                if target_cmd and cand_value is not None:
                    best_value = cand_value
                stall_count = 0
                bv = best_value if target_cmd else "n/a"
                log(f"iteration {iters}: accepted by LLM, new best (target={bv})")
                feedback.append(
                    f"  round {iters}: candidate {len(candidate)} chars -> ACCEPTED (target={bv})"
                )
            else:
                log(f"iteration {iters}: candidate rejected by LLM")
                feedback.append(
                    f"  round {iters}: candidate {len(candidate)} chars -> REJECTED by LLM"
                )
                write_file(src_path, best)
                stall_count += 1

        if stall_count >= stall_limit:
            log("stop: too many stalled iterations")
            break

    elapsed = time.time() - start
    bv = best_value if target_cmd else "n/a"
    log(f"done: {iters} iterations in {elapsed:.1f}s, final target value = {bv}")
    log(
        f"time breakdown: llm={t_llm:.1f}s eval={t_eval:.1f}s "
        f"judge={t_judge:.1f}s other={max(0.0, elapsed - t_llm - t_eval - t_judge):.1f}s"
    )
    if out_path != src_path:
        write_file(out_path, best)
    log(f"best result written to {out_path}")
    return {"content": best, "value": best_value, "iterations": iters, "elapsed": elapsed}


def main():
    parser = argparse.ArgumentParser(
        description="Optimize source code via opencode agent search.",
        usage="agentSearch.py [options] <src-path> <out-path> <feasible-cmd> [target-cmd]",
    )
    parser.add_argument("src_path", metavar="src-path")
    parser.add_argument("out_path", metavar="out-path")
    parser.add_argument("feasible_cmd", metavar="feasible-cmd")
    parser.add_argument(
        "target_cmd", nargs="?", default=None, metavar="target-cmd",
        help="Optional numeric objective; if omitted the LLM judges improvement.",
    )
    parser.add_argument("--config", required=True, help="Path to opencode config file")
    parser.add_argument("--model", default=None, help="Model reference (provider/model)")
    parser.add_argument("--auto", dest="auto", action="store_true", default=True)
    parser.add_argument("--no-auto", dest="auto", action="store_false")
    parser.add_argument("--max-iters", type=int, default=20, help="Default: 20")
    parser.add_argument("--stall", type=int, default=3, help="Default: 3")
    parser.add_argument("--timeout", type=float, default=300, help="Default: 300")
    parser.add_argument("--hint", default=None, help="Guidance for the optimization direction")
    parser.add_argument("--hint-file", default=None, help="Read guidance prompt from a file")
    parser.add_argument("--variant", default=None, help="Model reasoning effort (e.g. minimal, low, high)")
    args = parser.parse_args()

    hint = args.hint
    if args.hint_file:
        with open(args.hint_file, "r", encoding="utf-8") as f:
            hint = f.read()

    try:
        optimize(
            src_path=args.src_path,
            out_path=args.out_path,
            feasible_cmd=args.feasible_cmd,
            target_cmd=args.target_cmd,
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
        log_red(f"agentSearch.py: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
