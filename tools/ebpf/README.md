# eBPF tools

This directory contains tools for eBPF.

## Build

Dependencies: libbpf, bpftool, clang, llvm, gcc.

```bash
make
```

## Run

in one shell:

```bash
sudo ./et --print --fentry
```

then in another:

```bash
sudo cat /sys/kernel/debug/tracing/trace_pipe
```
