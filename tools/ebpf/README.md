# eBPF tools

This directory contains tools for eBPF.

## Build

Dependencies: libbpf, bpftool, clang, llvm, gcc, libelf, zlib.

```bash
make
```

## Run

fentry: a simple program to trace udp_send_skb calls, requires kernel > 5.5.

```bash
sudo ./et --prog fentry [--print]
```
