#!/usr/bin/env python3
"""Compile source files from sibling `<source>.make` command templates.

Usage:
    compile.py [-f|--force] [--default <path>] <srcPath> [envFile]

<srcPath> may be either a directory or a single file:

  - Directory: every `*.make` file under it is collected (recursively).
  - Single `.make` file: that one make file is used.
  - Single source file (e.g. foo.c): the sibling `foo.c.make` is used.

<envFile> is an optional `.env` file with `KEY=VALUE` lines (e.g. the
first line `CXX=/opt/llvm-21.1.0/bin/clang-21`). Variables from it are
used to expand `${KEY}` / `$KEY` references in the command template.

For each `.make` file, the prefix of its name is the source file and is
substituted for `$<`, e.g. `foo.c.make` treats `foo.c` as the source. The
template is a shell-like script:

    ${CXX} $< -Wno-everything -S -emit-llvm ... -o out.ll

Parsing rules: `$<` is replaced by the source file path, and `${KEY}` /
`$KEY` are expanded from the environment and the env file (in Python, not
by the shell, so the values can be printed), `$$` becomes a literal `$`.
The expanded text is then executed as `sh -c`, so multi-command scripts
(`&&`, pipelines, ...) work. The script writes its own output; no output
path or default extension is assumed. Runs are executed in the source
directory.

Unless `-f` / `--force` is given, the `-o <path>` argument is parsed from
the expanded command and, if that output exists and is at least as new as
both the source file and its `.make` template, the run is skipped as
up-to-date. If no `-o` path can be found, the command always runs.

Only the source and its `.make` template are compared. Header changes and
switching the env file are not detected; pass `-f` / `--force` to rebuild
in those cases.

The first time a variable is expanded, its name and value are printed as
`KEY: VALUE` (one per line), so the effective compiler/config is visible.

Source files with no sibling `*.make` file are skipped. Nothing is
rewritten.

An empty `.make` (whitespace only) falls back to the script given by
`--default <path>` (the file name is arbitrary); its mtime is then used
for the up-to-date check. Without `--default`, an empty `.make` is
skipped.
"""

import os
import pathlib
import re
import shlex
import shutil
import sys
import tempfile

from parallel import ParallelHelper, TaskResult, run_process

MAKE_EXT = ".make"
TIMEOUT_STEPS = (2.0, 8.0, 32.0, 128.0)


def make_to_source(make: pathlib.Path):
    """Return the source path for a .make file (the name without .make)."""
    return make.with_name(make.name[: -len(MAKE_EXT)])


def read_template(make: pathlib.Path):
    text = make.read_text(encoding="utf-8", errors="replace").strip()
    return text or None


def collect_makes(src_path: pathlib.Path):
    """Return list of .make files to process for a given srcPath."""
    if src_path.is_dir():
        return sorted(p for p in src_path.rglob("*") if p.is_file() and p.suffix == MAKE_EXT)
    if src_path.is_file():
        if src_path.suffix == MAKE_EXT:
            return [src_path]
        sibling = src_path.with_name(src_path.name + MAKE_EXT)
        return [sibling] if sibling.is_file() else []
    return []


def load_env(env_file: pathlib.Path):
    """Parse KEY=VALUE lines from an .env file into a dict."""
    env = {}
    for line in env_file.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        env[key.strip()] = value.strip().strip('"').strip("'")
    return env


_EXPANDED_ENV = set()


def expand_vars(text: str, env):
    """Expand `${KEY}` / `$KEY` using the given mapping.

    The first time a variable is expanded, print `KEY: VALUE` so the
    effective configuration is visible once per variable.
    """
    def repl(m):
        key = m.group(1) or m.group(2)
        if key not in env:
            return m.group(0)
        if key not in _EXPANDED_ENV:
            _EXPANDED_ENV.add(key)
            print(f"{key}: {env[key]}")
        return env[key]

    return re.sub(r"\$\{(\w+)\}|\$(\w+)", repl, text)


def build_cmd(src: pathlib.Path, template: str, env):
    return expand_vars(
        template.replace("$$", "$").replace("$<", str(src.resolve())), env
    )


def output_path(cmd_args, base_dir: pathlib.Path):
    """Best-effort extraction of the `-o <path>` output from a command."""
    out = None
    for i, tok in enumerate(cmd_args):
        if tok == "-o" and i + 1 < len(cmd_args):
            out = cmd_args[i + 1]
        elif tok.startswith("-o="):
            out = tok[len("-o="):]
    if out is None:
        return None
    path = pathlib.Path(out)
    return path if path.is_absolute() else base_dir / path


def is_up_to_date(out: pathlib.Path, *inputs: pathlib.Path):
    if not out.exists():
        return False
    try:
        out_mtime = out.stat().st_mtime
        return all(out_mtime >= p.stat().st_mtime for p in inputs)
    except OSError:
        return False


def run_one(src: pathlib.Path, cmd: str, env, timeout: float):
    """Run a command, returning a TaskResult."""
    proc = run_process(["sh", "-c", cmd], timeout, cwd=str(src.parent), env=env)
    if proc.timed_out:
        return TaskResult("TIMEOUT", src)
    if proc.returncode != 0:
        return TaskResult("FAIL", src, proc.stderr.strip()[:300])
    return TaskResult("OK", src)


def main():
    argv = sys.argv[1:]
    force = False
    default_make = None
    positional = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in ("-f", "--force"):
            force = True
        elif arg == "--default":
            if i + 1 >= len(argv):
                print("--default requires a path", file=sys.stderr)
                return 2
            default_make = pathlib.Path(argv[i + 1]).resolve()
            i += 1
        else:
            positional.append(arg)
        i += 1
    if len(positional) not in (1, 2):
        print(
            "usage: compile.py [-f|--force] [--default <path>] <srcPath> [envFile]",
            file=sys.stderr,
        )
        return 2
    src_path = pathlib.Path(positional[0])

    if default_make is not None and not default_make.is_file():
        print(f"default make not found: {default_make}", file=sys.stderr)
        return 2

    env = dict(os.environ)
    if len(positional) == 2:
        env_file = pathlib.Path(positional[1])
        if not env_file.is_file():
            print(f"env file not found: {env_file}", file=sys.stderr)
            return 2
        env.update(load_env(env_file))

    makes = collect_makes(src_path)
    if default_make is not None:
        makes = [m for m in makes if m.resolve() != default_make]
    if not makes:
        print(f"no .make files found for: {src_path}", file=sys.stderr)
        return 2

    if default_make is None:
        empty = [m for m in makes if not read_template(m)]
        if empty:
            print(
                f"empty template and no --default: {empty[0]}",
                file=sys.stderr,
            )
            print(f"({len(empty)} empty .make file(s))", file=sys.stderr)
            return 2

    tmp_root = tempfile.mkdtemp(prefix="compile-tmp-")
    env["TMPDIR"] = tmp_root

    ok = fail = skip = 0
    total = len(makes)
    finalized = 0

    def done_line():
        nonlocal finalized
        finalized += 1
        print(f"{finalized * 100 // total}%")

    runnable = []
    for make in makes:
        src = make_to_source(make)
        if not src.is_file():
            print(f"SKIP {make}: missing source {src}")
            skip += 1
            done_line()
            continue
        template = read_template(make)
        tmpl_file = make
        if not template and default_make is not None:
            template = read_template(default_make)
            if template:
                tmpl_file = default_make
        if not template:
            print(f"SKIP {make}: empty template, no default")
            skip += 1
            done_line()
            continue

        cmd = build_cmd(src, template, env)
        args = shlex.split(cmd)
        if not force:
            out = output_path(args, src.parent)
            if out is not None and is_up_to_date(out, src, tmpl_file):
                print(f"SKIP {src}: up-to-date ({out.name})")
                skip += 1
                done_line()
                continue
        runnable.append((src, cmd))

    def runner(item, timeout):
        src, cmd = item
        return run_one(src, cmd, env, timeout)

    def on_step(step, timeout, pending):
        print(f"timeout {timeout:g}s ({pending} pending)")

    def on_result(res):
        nonlocal ok, fail
        if res.status == "OK":
            print(f"OK   {res.item}")
            ok += 1
        elif res.status == "TIMEOUT":
            print(f"FAIL {res.item}: timeout after {TIMEOUT_STEPS[-1]:g}s")
            fail += 1
        else:
            print(f"FAIL {res.item}: {res.payload}")
            fail += 1
        done_line()

    ParallelHelper(timeout_steps=TIMEOUT_STEPS).run(
        runnable, runner, on_result, on_step=on_step
    )

    shutil.rmtree(tmp_root, ignore_errors=True)

    print(f"\ncompiled ok={ok} fail={fail} skip={skip}")


if __name__ == "__main__":
    sys.exit(main())
