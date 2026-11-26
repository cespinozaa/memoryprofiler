#!/bin/bash
# setup.sh: install everything memprof and its experiments need, on Ubuntu 24.04.
#
#   1. system packages (compilers, libbpf, bpftool, cmake, Python plotting)
#   2. check the kernel features memprof uses
#   3. GAPBS at the commit we used, built with debug info and frame pointers
#   4. llama.cpp at the commit we used, built twice:
#        ~/llama-completion-base            original
#        ~/llama.cpp/build/bin/llama-completion   with llama-lazy-clear.patch
#   5. download the model file if it is missing, and check its checksum
#
# Run it from the repository root:   ./setup.sh
# It uses sudo for apt. It does not build memprof itself (see "Build" in the README).
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
GAPBS_DIR="$HOME/gapbs"
GAPBS_COMMIT=941462c7c42e786e4e006a9211b0576ef78d23f7
LLAMA_DIR="$HOME/llama.cpp"
LLAMA_COMMIT=186eab1cb83899282bec44b837270959b56ee8f3
MODEL="$HOME/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf"
MODEL_SHA256=6f85a640a97cf2bf5b8e764087b1e83da0fdb51d7c9fab7d0fece9385611df83
MODEL_URL=https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf
JOBS="$(nproc)"

echo "== 1. System packages"
sudo apt-get update
sudo apt-get install -y build-essential clang llvm gcc-multilib pkg-config libbpf-dev libelf-dev zlib1g-dev linux-tools-common "linux-tools-$(uname -r)" binutils cmake git wget libcurl4-openssl-dev python3 python3-matplotlib python3-numpy

echo "== 2. Kernel features memprof needs"
for f in /sys/kernel/btf/vmlinux /proc/self/pagemap /proc/self/clear_refs /sys/kernel/mm/page_idle/bitmap; do
    if sudo test -e "$f"; then
        echo "ok       $f"
    else
        echo "MISSING  $f"
    fi
done
echo "(page_idle/bitmap is only needed for read tracking, MEMPROF_READS=1)"

echo "== 3. GAPBS (PageRank)"
if [ ! -d "$GAPBS_DIR" ]; then
    git clone https://github.com/sbeamer/gapbs.git "$GAPBS_DIR"
fi
git -C "$GAPBS_DIR" checkout "$GAPBS_COMMIT"
# Same flags as the Makefile's defaults, plus -g and -fno-omit-frame-pointer
# so memprof can walk the stack and addr2line can name source lines.
make -C "$GAPBS_DIR" -B pr CXX_FLAGS="-g -O3 -std=c++11 -Wall -fno-omit-frame-pointer -fopenmp"

echo "== 4. llama.cpp (original and patched)"
if [ ! -d "$LLAMA_DIR" ]; then
    git clone https://github.com/ggml-org/llama.cpp.git "$LLAMA_DIR"
fi
cd "$LLAMA_DIR"
# Undo the patch if this script already ran once, so the original can be rebuilt.
git checkout -- ggml/src/ggml-backend.cpp
git checkout "$LLAMA_COMMIT"
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS=-fno-omit-frame-pointer -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer
cmake --build build --target llama-completion -j"$JOBS"
cp build/bin/llama-completion "$HOME/llama-completion-base"
echo "original binary: $HOME/llama-completion-base"
git apply "$REPO/llama-lazy-clear.patch"
cmake --build build --target llama-completion -j"$JOBS"
echo "patched binary:  $LLAMA_DIR/build/bin/llama-completion"
cd "$REPO"

echo "== 5. Model file"
if [ ! -f "$MODEL" ]; then
    mkdir -p "$(dirname "$MODEL")"
    wget -O "$MODEL" "$MODEL_URL"
fi
echo "$MODEL_SHA256  $MODEL" | sha256sum -c -

echo "== Done. Next: build memprof (README, Build)."
