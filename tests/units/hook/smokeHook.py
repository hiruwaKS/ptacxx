#!/usr/bin/env python3
import subprocess, sys
def _global_symbols(so_path):
    out = subprocess.run(
        ["nm", "-D", "--defined-only", "-g", so_path],
        capture_output=True, text=True, timeout=30,
    )
    assert out.returncode == 0, f"nm exited with {out.returncode}"
    return out.stdout
def test_exported_symbols(so_path):
    out = _global_symbols(so_path)
    for sym in ["__hook_init", "__hook_push", "__hook_dump"]:
        assert sym in out, f"symbol {sym} missing from {so_path}"
    for line in out.split("\n"):
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        name = parts[2]
        assert not name.startswith("_Z"), \
            f"unexpected C++ symbol leaked: {name}"
test_exported_symbols(sys.argv[1])
