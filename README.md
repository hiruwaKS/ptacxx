# ptacxx

- notes
    - TODO: `Dockerfile`
    - for lotus, SVF, cclyzer++: use its own building script or cmake
    - we use the **suffix** of building directory to identify the tested target, which isolates the version differences
    - record `LOTUS_BINARY_DIR`, `SVF_BINARY_DIR` (usually ended with `Release-build`), `CCLYZERPP_BINARY_DIR`
    - you should assign `LLVM_DIR` to the one used in building lotus, SVF, cclyzer++, respectively
    - cmake configure step will run a complete toolchain test

### lotus

```bash
cmake -B /path/to/build/lotus -S . \
      -DCMAKE_C_COMPILER=/usr/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DLLVM_DIR=... \
      -DLOTUS_BINARY_DIR=...
```

### SVF

```bash
cmake -B /path/to/build/svf -S . \
      -DCMAKE_C_COMPILER=/usr/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DLLVM_DIR=... \
      -DSVF_BINARY_DIR=...
```

### cclyzer++

```bash
cmake -B /path/to/build/lotus -S . \
      -DCMAKE_C_COMPILER=/usr/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DLLVM_DIR=... \
      -DCCLYZERPP_BINARY_DIR=...
```
