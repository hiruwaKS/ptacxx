#!/usr/bin/env python3
"""Compile source files from sibling `<source>.make` command templates.

Usage:
    compile.py <srcPath>

<srcPath> may be either a directory or a single file:

  - Directory: every `*.make` file under it is collected (recursively).
  - Single `.make` file: that one make file is used.
  - Single source file (e.g. foo.c): the sibling `foo.c.make` is used.

For each `.make` file, the prefix of its name is the source file and is
substituted for `$<` in the command template, e.g. `foo.c.make` treats
`foo.c` as the source. The template is a single command line:

    clang-21 $< -Wno-everything -S -emit-llvm ... -o out.ll

Parsing rules: `$<` is replaced by the source file path, and `$$` by a
literal `$`. The command itself writes its own output; no output path or
default extension is assumed. Runs are executed in the source directory.

Source files with no sibling `*.make` file are skipped. Nothing is
rewritten.
"""

import pathlib
import shlex
import subprocess
import sys

MAKE_EXT = ".make"


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


def build_cmd(src: pathlib.Path, template: str):
    return (
        template.replace("$$", "$").replace("$<", str(src))
    )


def main():
    if len(sys.argv) != 2:
        print("usage: compile.py <srcPath>", file=sys.stderr)
        return 2
    src_path = pathlib.Path(sys.argv[1])

    makes = collect_makes(src_path)
    if not makes:
        print(f"no .make files found for: {src_path}", file=sys.stderr)
        return 2

    ok = fail = skip = 0
    for make in makes:
        src = make_to_source(make)
        if not src.is_file():
            print(f"SKIP {make}: missing source {src}")
            skip += 1
            continue
        template = read_template(make)
        if not template:
            print(f"SKIP {make}: empty template")
            skip += 1
            continue

        cmd = build_cmd(src, template)
        r = subprocess.run(shlex.split(cmd), cwd=str(src.parent), capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {src}: {r.stderr.strip()[:300]}")
            fail += 1
        else:
            print(f"OK   {src}")
            ok += 1

    print(f"\ncompiled ok={ok} fail={fail} skip={skip}")


if __name__ == "__main__":
    sys.exit(main())
