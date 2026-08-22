# ptacxx

- A practical, unified, interactive and easy-to-use wrapper that integrates C/C++ pointer analysis frameworks, supporting Lotus, SVF, and CCLyzer++
- For practitioners in code auditing and program analysis, it streamlines the entire analysis workflow and accelerates daily tasks
- For framework developers, it helps quickly pinpoint issues within the analysis engines

## Build

- TODO: `Dockerfile`

### Choose and build backend

| Name | Description |
| --- | --- |
| Lotus | program analysis, verification, and optimization framework |
| SVF | static value-flow analysis tool for LLVM-based languages |
| CCLyzer++ | precise and scalable global pointer analysis for LLVM code |

- for Lotus, SVF, CCLyzer++: use its own building script or cmake to build
- for "debug configure", see the last part

### Configure

- we use the **suffix** (one of `lotus`, `svf`, `cclyzerpp`) of building directory to identify the backend, which isolates the version differences

```bash
mkdir /path/to/build/<suffix>
cmake -B /path/to/build/<suffix> -S . \
      -DCMAKE_C_COMPILER=/usr/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DCMAKE_AR=/usr/bin/llvm-ar \
      -DLLVM_DIR=... \
      -DBACKEND_BINARY_DIR=...
```
- `CMAKE_CXX_COMPILER` should be the same with the compiler you use in compiling the backend. Actually, you can use gcc if you already use gcc to compile backend and don't need the "debug configure"
- `LLVM_DIR` should be assigned to the one used in configuring backend respectively. Notably, `CMAKE_CXX_COMPILER` (and similar variables) can be of different versions with `LLVM_DIR`
- `BACKEND_BINARY_DIR` should be the directory where the backend binary is located, usually ended with `build` or `Release-build`
> cmake configure step will run a complete toolchain test

## Usage

- TODO

## For Framework Developers

- debug configure: build the program like a tested input, extract its bitcode
- compile the backend using the compiler with the same version with `LLVM_DIR`. Add `"-g -O0 -flto=thin"`
- use `target help` to find the tested backend with ``_bc`` suffix, build and locate the `.bc` file

```bash
cmake -B <build_dir> -S . \
      -DCMAKE_C_COMPILER=<the same version with LLVM_DIR>\
      -DCMAKE_CXX_COMPILER=<the same version with LLVM_DIR> \
      -DCMAKE_AR=<the same version with LLVM_DIR> \
      -DBACKEND_BINARY_DIR=... \
      -DLLVM_DIR=... \
      -DCMAKE_C_FLAGS="-g -O0 -flto=thin" \
      -DCMAKE_CXX_FLAGS="-g -O0 -flto=thin"
```

## TODOS

- add version control support and make notes shareable