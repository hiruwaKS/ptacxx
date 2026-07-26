# ptacxx

- notes
    - TODO: `Dockerfile`
    - for lotus, SVF: use its own building script or cmake
    - record `LOTUS_BINARY_DIR`, `SVF_BINARY_DIR` (usually ended with `Release-build`), and the two `LLVM_DIR`s
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
