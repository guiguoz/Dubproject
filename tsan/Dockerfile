# ── Image unique : Ubuntu 24.04 + Clang 18 + TSan ──────────────────────────
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang-18 \
    libclang-rt-18-dev \
    llvm-18 \
    cmake \
    make \
    git \
    ca-certificates \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Symlinks pour que TSan et le build trouvent les outils
RUN update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 \
    && ln -s /usr/bin/llvm-symbolizer-18 /usr/bin/llvm-symbolizer

WORKDIR /build

# ── Copier le projet ────────────────────────────────────────────────────────
COPY . /build/project

WORKDIR /build/project

# ── CMake configure + build ─────────────────────────────────────────────────
RUN cmake -S tsan -B build-tsan \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build-tsan --target EngineTSan --parallel $(nproc)

# ── Défaut : exécuter les tests TSan ───────────────────────────────────────
ENTRYPOINT ["./build-tsan/EngineTSan"]
CMD ["[tsan]"]
