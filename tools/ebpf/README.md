# eBPF based tools for MTL

This directory contains eBPF based tools for MTL.

## 1. Dependencies

Dependencies: libbpf, bpftool, libxdp, clang, llvm, gcc, libelf, zlib.

If you has problems to install the dependencies for libbpf, bpftool, or libxdp, you might need to install these dependencies manually, follow libbpf page <https://github.com/libbpf/libbpf>, bpftool page <https://github.com/libbpf/bpftool>, libxdp page <https://github.com/xdp-project/xdp-tools> to build and install from source code directly.

## 2. Run

### 2.1. lcore_monitor

lcore_monitor: a eBPF based tool to monitor the scheduler and interrupt event on one MTL lcore. The MTL achieves high data packet throughput and low latency by defaulting to busy polling, also referred to as busy-waiting or spinning.
To ensure peak performance, it is crucial to verify that no other tasks are running on the same logical core (lcore) that MTL utilizes. For debugging purposes, the tool provides a status overview via an eBPF (extended Berkeley Packet Filter) trace point hook, which monitors the activities on a single core.

Build:

```bash
make lcore_monitor
```
Run:

```bash
sudo ./lcore_monitor --lcore 30 --t_pid 194145
```

The output is like below, inspect the time to check if the lcore is suspending for a long time.

```text
main, load bpf object lcore_monitor_kern.o succ
lm_event_handler: sched out 7.789us as comm: migration/30
lm_event_handler: sched out 7.405us as comm: migration/30
```

The `lcore` and `t_pid` can be get from MTL running log.

```text
MT: MT: 2024-01-17 15:45:14, * *    M T    D E V   S T A T E   * *
MT: MT: 2024-01-17 15:45:14, DEV(0): Avr rate, tx: 2610.440314 Mb/s, rx: 0.000278 Mb/s, pkts, tx: 2465879, rx: 6
MT: MT: 2024-01-17 15:45:14, DEV(1): Avr rate, tx: 0.000000 Mb/s, rx: 2602.470600 Mb/s, pkts, tx: 0, rx: 2465811
MT: MT: 2024-01-17 15:45:14, SCH(0:sch_0): tasklets 3, lcore 29(t_pid: 190637), avg loop 105 ns
MT: MT: 2024-01-17 15:45:14, SCH(1:sch_1): tasklets 1, lcore 30(t_pid: 190638), avg loop 45 ns
```

### 2.2. udp_monitor

udp_monitor: a eBPF based tool to monitor UDP streaming on the network, please note the promiscuous mode will be enabled on the selected interface.
"promiscuous mode" means that a network interface card will pass all packets received up to the software for processing, versus the traditional mode of operation wherein only packets destined for the NIC's MAC address are passed to software. Generally, promiscuous mode is used to "sniff" all traffic on the wire.

Build:

```bash
make udp_monitor
```
Run:

```bash
sudo ./udp_monitor --interface enp175s0f0np0 --dump_period_s 5
```

The dump output is like below:

```text
----- DUMP UDP STAT EVERY 5s -----
192.168.17.101:19873 -> 239.168.17.101:20000, 2610.896736 Mb/s pkts 1233168
192.168.17.102:19941 -> 239.168.17.102:20000, 2610.922137 Mb/s pkts 1233180
192.168.17.101:30073 -> 239.168.17.101:30000, 1.967981 Mb/s pkts 5000
192.168.17.102:29970 -> 239.168.17.102:30000, 1.967981 Mb/s pkts 5000
192.168.17.102:40003 -> 239.168.17.102:40000, 0.102719 Mb/s pkts 300
192.168.17.101:39925 -> 239.168.17.101:40000, 0.102719 Mb/s pkts 300
```
