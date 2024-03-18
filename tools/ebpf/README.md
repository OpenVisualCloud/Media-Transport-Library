# eBPF based tools for IMTL

This directory contains eBPF based tools for IMTL.

## 1. Dependencies

Dependencies: libbpf, bpftool, libxdp, clang, llvm, gcc, libelf, zlib.

If you has problems to install the dependencies for libbpf, bpftool, or libxdp, you might need to install these dependencies manually, follow libbpf page <https://github.com/libbpf/libbpf>, bpftool page <https://github.com/libbpf/bpftool>, libxdp page <https://github.com/xdp-project/xdp-tools> to build and install from source code directly.

## 2. Build

```bash
make
```

## 3. Run

### 3.1 lcore_monitor

lcore_monitor: a tool to monitor the scheduler and interrupt event on one MTL lcore. The IMTL achieves high data packet throughput and low latency by defaulting to busy polling, also referred to as busy-waiting or spinning.
To ensure peak performance, it is crucial to verify that no other tasks are running on the same logical core (lcore) that IMTL utilizes. For debugging purposes, the tool provides a status overview via an eBPF (extended Berkeley Packet Filter) trace point hook, which monitors the activities on a single core.

```bash
sudo ./lcore_monitor --lcore 30 --t_pid 194145
```

The output is like below, inspect the time to check if the lcore is suspending for a long time.

```bash
main, load bpf object lcore_monitor_kern.o succ
lm_event_handler: sched out 7.789us as comm: migration/30
lm_event_handler: sched out 7.405us as comm: migration/30
```

The `lcore` and `t_pid` can be get from IMTL running log.

```bash
MT: MT: 2024-01-17 15:45:14, * *    M T    D E V   S T A T E   * *
MT: MT: 2024-01-17 15:45:14, DEV(0): Avr rate, tx: 2610.440314 Mb/s, rx: 0.000278 Mb/s, pkts, tx: 2465879, rx: 6
MT: MT: 2024-01-17 15:45:14, DEV(1): Avr rate, tx: 0.000000 Mb/s, rx: 2602.470600 Mb/s, pkts, tx: 0, rx: 2465811
MT: MT: 2024-01-17 15:45:14, SCH(0:sch_0): tasklets 3, lcore 29(t_pid: 190637), avg loop 105 ns
MT: MT: 2024-01-17 15:45:14, SCH(1:sch_1): tasklets 1, lcore 30(t_pid: 190638), avg loop 45 ns
```

### 3.2 fentry

fentry: a simple program to trace udp_send_skb calls, requires kernel > 5.5.

```bash
sudo ./et --prog fentry [--print]
```

xdp: a privileged program to load custom xdp bpf program:

```bash
sudo ./et --prog xdp --ifname ens785f0,ens785f1 --xdp_path your.xdp.o
```
