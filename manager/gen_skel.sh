#!/bin/sh

cp "$1"/manager.xdp.c .

clang -g -O2 -target bpf -c manager.xdp.c -o manager.xdp.o
llvm-strip -g manager.xdp.o
bpftool gen skeleton manager.xdp.o > xdp.skel.h
