# eBPF tools

This directory contains tools for eBPF.

## Build

Dependencies: libbpf, libxdp, bpftool, clang, llvm, gcc, libelf, zlib.

```bash
make
```

## Run

fentry: a simple program to trace udp_send_skb calls, requires kernel > 5.5.

```bash
sudo ./et --prog fentry [--print]
```

xdp: a privileged program to load afxdp bpf program and send the xsk_map_fd to other processes by socket.

```bash
sudo ./et --prog xdp --ifname ens785f0
```
