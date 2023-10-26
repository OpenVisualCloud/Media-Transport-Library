# eBPF tools

This directory contains tools for eBPF.

## Build

Dependencies: libbpf, bpftool, clang, llvm, gcc, libelf, zlib.

```bash
make
```

## Run

```bash
sudo ./et --prog fentry [--print]
```
