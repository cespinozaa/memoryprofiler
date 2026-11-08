#!/bin/bash
# usage: ./build.sh NAME   builds NAME.bpf.c, then NAME.c if there is one
set -e
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I. -c "$1.bpf.c" -o "$1.bpf.o"
if [ -f "$1.c" ]; then
    bpftool gen skeleton "$1.bpf.o" > "$1.skel.h"
    clang -g -O2 -Wall -I. "$1.c" -lbpf -lelf -lz -o "$1"
fi
