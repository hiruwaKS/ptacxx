#!/usr/bin/env python3
import subprocess, sys, tempfile, os

def _compile_to_bc(clang_bin, src, d):
    bc = os.path.join(d, os.path.basename(src) + ".bc")
    subprocess.run(f"{clang_bin} -emit-llvm -c -O0 -g -o {bc} {src}",
                   shell=True, check=True, capture_output=True, text=True, timeout=30)
    return bc

def _query_irm(irm_bin, bc_path, query):
    p = subprocess.run(f"{irm_bin} {bc_path}", shell=True,
                       input=query, capture_output=True, text=True, timeout=30)
    assert p.returncode == 0, f"irm exited with {p.returncode}\nstderr: {p.stderr}"
    return p.stdout.strip()

def _vid_from_name(name_out):
    for line in name_out.split("\n"):
        line = line.strip()
        if line and not line.startswith("<"):
            return line.split()[0]
    raise ValueError(f"no VId found in:\n{name_out}")

def _vid_with_local(vid, local_idx):
    parts = vid.split(":")
    parts[-1] = str(local_idx)
    return ":".join(parts)

def _llvm_major(clang_bin):
    out = subprocess.run(f"{clang_bin} -dumpversion", shell=True,
                         capture_output=True, text=True)
    return int(out.stdout.strip().split(".")[0])

def testInput1(irm_bin, clang_bin, src):
    with tempfile.TemporaryDirectory(prefix="irm_smoke_") as d:
        bc = _compile_to_bc(clang_bin, src, d)
        
        out = _query_irm(irm_bin, bc, "meta")
        print(f"> meta:\n{out}\n")
        assert len(out.split("\n")) >= 5
        for k in ["moduleID:", "sourceFileName:", "targetTriple:"]:
            assert k in out
        
        out = _query_irm(irm_bin, bc, "stat")
        print(f"> stat:\n{out}\n")
        for k in ["hasMain: 1", "hasGlobalCtor: 0", "hasGlobalDtor: 0", "globalCnt: 0"]:
            assert k in out
        
        out = _query_irm(irm_bin, bc, "name func2")
        print(f"> name func2:\n{out}\n")
        assert "func2" in out
        vid = _vid_from_name(out)
        
        out = _query_irm(irm_bin, bc, f"debug {vid}")
        print(f"> debug {vid}:\n{out}\n")
        assert "func2" in out and "smokeIRMInput.cpp" in out
        
        out = _query_irm(irm_bin, bc, "name main")
        print(f"> name main:\n{out}\n")
        assert "main" in out
        vid = _vid_from_name(out)
        
        out = _query_irm(irm_bin, bc, f"debug {vid}")
        print(f"> debug {vid}:\n{out}\n")
        assert "main" in out and "smokeIRMInput.cpp" in out

def testInput2(irm_bin, clang_bin, src):
    ver = _llvm_major(clang_bin)
    with tempfile.TemporaryDirectory(prefix="irm_smoke_") as d:
        bc = _compile_to_bc(clang_bin, src, d)
        
        out = _query_irm(irm_bin, bc, "name ptr")
        print(f"> name ptr:\n{out}\n")
        assert "ptr" in out
        vid = _vid_from_name(out)
        
        out = _query_irm(irm_bin, bc, f"debug {vid}")
        print(f"> debug {vid}:\n{out}\n")
        assert "Function" in out and "ptr" in out
        
        out = _query_irm(irm_bin, bc, f"debug {_vid_with_local(vid, 1)}")
        print(f"> debug {_vid_with_local(vid, 1)}:\n{out}\n")
        assert "%0" in out
        assert "Argument:i32*" in out if ver < 15 else "Argument:ptr" in out
        
        out = _query_irm(irm_bin, bc, f"debug {_vid_with_local(vid, 2)}")
        print(f"> debug {_vid_with_local(vid, 2)}:\n{out}\n")
        assert "%2" in out
        assert "Instruction:i32**" in out if ver < 15 else "Instruction:ptr" in out
        
        out = _query_irm(irm_bin, bc, "stat")
        print(f"> stat:\n{out}\n")
        assert "hasMain: 1" in out and "hasGlobalCtor: 1" in out

def main():
    irm_bin, clang_bin = sys.argv[1], sys.argv[2]
    if len(sys.argv) >= 4: testInput1(irm_bin, clang_bin, sys.argv[3])
    if len(sys.argv) >= 5: testInput2(irm_bin, clang_bin, sys.argv[4])

if __name__ == "__main__":
    main()