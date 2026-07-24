# use Docker build cache, avoid inserting command before

# =========================================================
# Stage 1: base (Source Management & Download Tools)
# Purpose: Provides lightweight tools needed for cloning & downloading
# =========================================================
FROM ubuntu:22.04 AS base

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    git \
    xz-utils \
    tar \
    && rm -rf /var/lib/apt/lists/*


# =========================================================
# Stage 2: llvm-multi
# Purpose: Fetches LLVM 14 source code & pre-built LLVM 21
# =========================================================
FROM base AS llvm-multi

# 1. Clone LLVM 14 (sparse checkout, only llvm subdirectory, if failed, check CONNECTION)
# Using timeout to prevent infinite hang
RUN echo "Cloning LLVM 14..." && \
    timeout 600 git clone --filter=blob:none --depth 1 \
        --branch llvmorg-14.0.6 \
        https://github.com/llvm/llvm-project.git /opt/llvm-14 \
    || (echo "Clone failed, retrying..." && rm -rf /opt/llvm-14 && \
        git clone --filter=blob:none --depth 1 \
            --branch llvmorg-14.0.6 \
            https://github.com/llvm/llvm-project.git /opt/llvm-14) \
    && cd /opt/llvm-14 \
    && git sparse-checkout init --cone \
    && git sparse-checkout set llvm \
    && git checkout --progress

# 2. Download and extract pre-built LLVM 21 (RTTI-enabled)
RUN echo "Downloading LLVM 21..." && \
    curl -fL --retry 5 --retry-delay 10 \
        --connect-timeout 30 \
        --speed-limit 1000 --speed-time 30 \
        --progress-bar \
        https://github.com/bjjwwang/SVF-LLVM/releases/download/21.1.0/llvm-21.1.0-ubuntu22-rtti-x86-64.tar.gz \
        -o /llvm21.tar.gz \
    && tar -xf /llvm21.tar.gz -C /opt/ \
    && mv /opt/llvm-21.1.0.obj /opt/llvm-21 \
    && rm /llvm21.tar.gz


# =========================================================
# Stage 3: ptacxx-env
# Purpose: Adds build tools, library dependencies, & testing/debug utilities
# =========================================================
FROM llvm-multi AS ptacxx-env

# ==========================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgmp-dev \
    libz3-dev \
    libzstd-dev \
    zlib1g-dev \
    libxml2-dev \
    libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

# ==========================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgtest-dev \
    libgmock-dev \
    gcc \
    g++ \
    cmake \
    ninja-build \
    make \
    gdb \
    patchelf \
    file \
    llvm-14-tools \
    python3 \
    python3-pip \
    python3-pytest \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --upgrade pip

# ==========================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    csmith \
    && rm -rf /var/lib/apt/lists/*

# ==========================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    libnng-dev \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install pynng --no-cache-dir

# DON'T set env in Dockerfile, write in compose.yml
# no ENV ...

# this will sync with the host env, not a copy
WORKDIR /repos

CMD ["bash"]