# eBPF tools

This directory contains tools for eBPF.

## Build

Dependencies: libbpf, libxdp, bpftool, clang, llvm, gcc, libelf, zlib.

```bash
make
```

## Run

lcore_monitor: a tools to monitor the scheduler even on the IMTL lcore.

```bash
sudo ./lcore_monitor --lcore 30 --t_pid 194145 --bpf_prog lcore_monitor_kern.o
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

fentry: a simple program to trace udp_send_skb calls, requires kernel > 5.5.

```bash
sudo ./et --prog fentry [--print]
```

xdp: a privileged program to load custom xdp bpf program:

```bash
sudo ./et --prog xdp --ifname ens785f0,ens785f1 --xdp_path your.xdp.o
```
