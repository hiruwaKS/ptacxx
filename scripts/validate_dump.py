#!/usr/bin/env python3
"""Validate a PtaHook dump file written by PtaHook::dump().

Sections recognized, in any order:
  pts        ::= "pts" NEWLINE ( <key> ( <t> [ "{" <ctx> "}" ] )* )*
  basicBlock ::= "basicBlock" NEWLINE ( <vid> ( " " <vid> )* )*   # BBCtxPlusOne vids
  callGraph  ::= "callGraph" NEWLINE ( <record> ( ", " <record> )* )*
                 record ::= <vid> ( " " arg )*        # CGCtxPlusOne records
                 arg    ::= POS | ZERO | NEG | NULL | "{" <typevids> "}"

All numbers are signed 64-bit integers. Section widths (BBCtxPlusOne,
CGCtxPlusOne) are inferred from the first data line, then enforced on every
line of that section.

Usage: validate_dump.py <dump-file>
Exit 0 on a well-formed dump, 1 otherwise.
"""

import re
import sys

INT = re.compile(r"-?\d+")
KINDS = {"POS", "ZERO", "NEG", "NULL"}
HEADERS = {"pts", "basicBlock", "callGraph"}


def bad_uint_line(line):
    # any token that is not an integer (signed) => malformed
    return any(not INT.fullmatch(t) for t in line.split())


def parse_record(rec, sec, line_no, errors, debug):
    """Validate one callGraph record. Returns True if well-formed."""
    toks = rec.split()
    if not toks:
        _e(sec, line_no, "empty record", errors)
        return False
    if not INT.fullmatch(toks[0]):
        _e(sec, line_no, f"bad vid '{toks[0]}' in record", errors)
        return False
    j = 1
    while j < len(toks):
        t = toks[j]
        if t in KINDS:
            j += 1
            continue
        if t == "{":
            k = j + 1
            while k < len(toks) and toks[k] != "}":
                if not INT.fullmatch(toks[k]):
                    _e(sec, line_no, f"bad type vid '{toks[k]}' in '{{...}}'", errors)
                    return False
                k += 1
            if k >= len(toks):
                _e(sec, line_no, "unclosed '{' in record", errors)
                return False
            if "{" in toks[j + 1:k]:
                _e(sec, line_no, "nested '{' in record", errors)
                return False
            j = k + 1
            continue
        _e(sec, line_no, f"unexpected token '{t}' in record", errors)
        return False
    return True


def _e(sec, line_no, msg, errors, debug=True):
    if debug:
        errors.append(f"{sec} line {line_no}: {msg}")


def _skip_to_header(lines, idx):
    while idx < len(lines) and lines[idx].strip() not in HEADERS:
        idx += 1
    return idx


def validate_pts(lines, idx, errors, debug):
    """lines[idx] == 'pts'. Validate until next header or EOF."""
    idx += 1
    while idx < len(lines):
        line = lines[idx].rstrip()
        if not line:
            idx += 1
            continue
        if line in HEADERS:
            break
        toks = line.split()
        ok = INT.fullmatch(toks[0])  # key
        j = 1
        while ok and j < len(toks):
            if not INT.fullmatch(toks[j]):
                ok = False
                break
            j += 1
            if j < len(toks) and toks[j] == "{":
                k = j + 1
                while k < len(toks) and toks[k] != "}":
                    if not INT.fullmatch(toks[k]):
                        ok = False
                        break
                    k += 1
                if ok and k >= len(toks):
                    ok = False
                if ok:
                    j = k + 1
        if not ok:
            _e("pts", idx, f"malformed line '{line}'", errors)
            return _skip_to_header(lines, idx)
        idx += 1
    return idx


def validate_basicblock(lines, idx, errors, debug):
    idx += 1
    width = None
    while idx < len(lines):
        line = lines[idx].rstrip()
        if not line:
            idx += 1
            continue
        if line in HEADERS:
            break
        if bad_uint_line(line):
            _e("basicBlock", idx, f"malformed line '{line}'", errors)
            return _skip_to_header(lines, idx)
        n = len(line.split())
        if width is None:
            width = n
        elif n != width:
            _e("basicBlock", idx, f"line has {n} vids, expected {width}", errors)
            return _skip_to_header(lines, idx)
        idx += 1
    return idx


def validate_callgraph(lines, idx, errors, debug):
    idx += 1
    width = None
    while idx < len(lines):
        line = lines[idx].rstrip()
        if not line:
            idx += 1
            continue
        if line in HEADERS:
            break
        records = line.split(", ")
        if width is None:
            width = len(records)
        elif len(records) != width:
            _e("callGraph", idx, f"line has {len(records)} records, expected {width}", errors)
            return _skip_to_header(lines, idx)
        for rec in records:
            if not parse_record(rec, "callGraph", idx, errors, debug):
                return _skip_to_header(lines, idx)
        idx += 1
    return idx


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <dump-file>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        print(f"error: cannot read {path}: {e}", file=sys.stderr)
        return 2

    lines = text.splitlines()
    errors = []
    sections = {"pts": 0, "basicBlock": 0, "callGraph": 0}

    # global cleanliness: no NUL / control chars except tab/newline
    for ln, line in enumerate(lines, 1):
        for ch in line:
            if ch != "\t" and (ord(ch) < 32 or ord(ch) > 126):
                _e("global", ln, f"non-printable char U+{ord(ch):04X}", errors)

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            i += 1
            continue
        if line in HEADERS:
            sections[line] += 1
            if line == "pts":
                i = validate_pts(lines, i, errors, debug=True)
            elif line == "basicBlock":
                i = validate_basicblock(lines, i, errors, debug=True)
            else:
                i = validate_callgraph(lines, i, errors, debug=True)
            continue
        _e("global", i + 1, f"unexpected top-level content '{line}'", errors)
        i += 1

    for sec, count in sections.items():
        print(f"{sec}: {count} section(s)")

    if errors:
        print(f"\n{len(errors)} problem(s) found:", file=sys.stderr)
        for e in errors[:50]:
            print(f"  {e}", file=sys.stderr)
        if len(errors) > 50:
            print(f"  ... and {len(errors) - 50} more", file=sys.stderr)
        return 1

    print("OK: dump is well-formed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
