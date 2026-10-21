# Performance Benchmark for CVL

## 1. Benchmark server setup

Server Model: M50CYP2SBSTD  
Processor Information: Intel® Xeon® Platinum 8358 CPU @ 2.60GHz
                       2 Processor, 64 Cores  
Memory: 16 x 32G (DIMM DDR4 Synchronous 3200 MHz)  
NIC: CVL 100G  
APP used: RxTxApp reference app.

## 2. Benchmark result

### 2.1. Transmitter in frame mode

following table shows the benchmark of max tx session number.

format | 1 port with 1 NIC | 2 port with 2 NIC
--- | --- | ---
720p29.97 | 160 | 160 x 2
720p50 | 96 | 96 x 2
720p59.94 | 80 | 80 x 2
1080p29.97 | 72 | 72 x 2
1080p50 | 45 | 38 x 2
1080p59.94 | 37 | 32 x 2
2160p29.97 | 19 | 18 x 2
2160p50 | 12 | 11 x 2
2160p59.94 | 9 | 9 x 2
4320p59.94 | 2 | 2 x 2

### 2.2. Transmitter + Receiver in frame mode for 8K & 4k

config | max-sessions | comment
--- | --- | ---
2160p59_Tx | 9 | 1port on 1NIC
2160p59_4Tx | 28 | 4port on 4NIC
2160p59_Tx + Rx​[Tx (Socket0), Rx (Socket1)] | 9Tx+9RX | 2port on 2NIC
2160p59_2Tx + 2Rx​[Tx (Socket0, Socket1), Rx (Socket1, Socket0)] | 18Tx+18RX | 4port on 4NIC
4320p59_Tx | 2 | 1port on 1NIC
4320p59_4Tx | 8 | 4port on 4NIC
4320p59_Tx + Rx​[Tx (Socket0), Rx (Socket1)] | 2TX+2RX | 2port on 2NIC
4320p59_2Tx + 2Rx​[Tx (Socket0, Socket1), Rx (Socket1, Socket0)] | 4TX+4RX | 4port on 4NIC

## 3. Measure DDR performance

Intel® Memory Latency Checker (Intel® MLC) <https://www.intel.com/content/www/us/en/download/736633/intel-memory-latency-checker-intel-mlc.html> is a tool used to measure memory latencies and b/w, and how they change with increasing load on the system.

The performance of MTL is bound by memory throughput, with dependencies on DDR bandwidth and Last Level Cache (LLC). If you experience issues related to core density, it is recommended to run `Intel® Memory Latency Checker (Intel® MLC)` to diagnose any memory system bottlenecks, such as unpopulated DDR DIMM slots.

Below is the output from Intel® MLC Bandwidths on our reference setup:

```bash
./Linux/mlc
```
```text
Measuring Peak Injection Memory Bandwidths for the system
Bandwidths are in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)
Using all the threads from each core if Hyper-threading is enabled
Using traffic with the following read-write ratios
ALL Reads        :      223526.3
3:1 Reads-Writes :      205940.2
2:1 Reads-Writes :      203939.4
1:1 Reads-Writes :      193479.6
Stream-triad like:      182056.7
```

## 4. RX video packet prefetch

`rv_handle_mbuf()` takes a burst of mbufs from the queue and dispatches them to the packet handler one at a time. Every handler starts by reading the RTP header at the front of the mbuf, and that line is cold: the NIC wrote it by DMA, so the core takes a miss on the first access of each packet and the burst serializes on those misses.

The dispatch loop prefetches the next packet of the burst while the current one is still being handled, so its header line is on the way in by the time the handler reaches it. A second prefetch covers the line behind the header, where the payload starts.

The payload prefetch is issued only for a session whose handler copies the payload on the CPU out of this mbuf. A header split session copies out of `mbuf->next` instead, and a DMA session hands every payload large enough to matter to the device; it still copies the small, cross page and `mt_dma_full()` ones on the CPU, but those are not worth a per packet test.

### 4.1. Build option

`enable_rx_video_pkt_prefetch` controls it, `true` by default.

```bash
# turn it off in the existing library build directory
meson configure build -Denable_rx_video_pkt_prefetch=false
ninja -C build

# or configure a separate one to compare the two side by side
meson setup build_no_prefetch -Denable_rx_video_pkt_prefetch=false
ninja -C build_no_prefetch
```

With the option off the dispatch loop issues no prefetch and every handler takes its own first access miss. A prefetch pays off only while the core has work to overlap with the fetch, so build both ways and compare on the target platform instead of assuming the default suits it.

## 5. RX video frame copy with non-temporal stores

An RX video frame is write-only until the application consumes it, so an ordinary store spends a read-for-ownership on every destination cache line: it reads a line the copy is about to overwrite in full, and evicts the RX mbufs that the following packets still have to be read from.

`rv_frame_memcpy()` streams whole destination cache lines with non-temporal stores instead, and leaves the misaligned head, the sub-line tail and short payloads on `mt_memcpy()`, because a partially filled write-combining buffer flushes as a read-modify-write that costs more than the read-for-ownership it replaces.

Two cases never reach the streaming path, so the option does not move them. A session with a pkt lcore keeps ordinary stores, because two threads write its frame and a fence orders only the stores of the core that runs it. With DSA offload active, every payload above `ST_RX_VIDEO_DMA_MIN_SIZE` is copied by the device.

What keeps the copy relevant under DSA is `mt_dma_full()` backpressure, line-crossing packets, and st22, which has no DMA branch at all.

### 5.1. Build option

`enable_rx_video_nt_store` controls it, `true` by default. The streaming path is compiled only when the target has SSE2, so on a non-x86 build the option is already a no-op.

```bash
# turn it off in the existing library build directory
meson configure build -Denable_rx_video_nt_store=false
ninja -C build

# or configure a separate one to compare the two side by side
meson setup build_no_nt -Denable_rx_video_nt_store=false
ninja -C build_no_nt
```

With the option off every RX video frame copy goes through `mt_memcpy()` and the three store fences that publish a frame to the application are compiled out with it. Build both ways and compare `incomplete frames` at a fixed session count to tell a copy-bound RX regression apart from a pacing or scheduling one.

## 6. Host and application setup for low latency

Each item of this section compares one change with the setup before it, and keeps the rest the same. Item 6.5 changes two settings together.
The host is 2 x Xeon Gold 6348 (Ice Lake) with an E810-C, the TX and the RX VF on one port, TX and RX in one process. Item 6.1 was also measured on Granite Rapids (2 x Xeon 6767P, E830). All sessions use `*_TX_FLAG_USER_PACING`. Run the commands of this section as root. Two workloads:

* client mix: 1 x 2160p59.94 and 2 x 1080p59.94 st20p, 8 st30p (audio) and 16 st40p (ANC) sessions (items 6.1, 6.2 and the switch count of 6.3).
* full mix: 2 x 2160p59.94, 4 x 1080p59.94 and 8 x 720p59.94 st20p, 16 st30p (audio, 1 ms frames) and 32 st40p (ANC) sessions, 40.6 Gb/s TX and 40.6 Gb/s RX, also with `*_TX_FLAG_USER_TIMESTAMP` (the other items).

The test application writes the media instant and the create time into each payload, so the receiver measures each frame against its media instant.

In the full mix, the lcores are 1-15, and their HT siblings 57-71 stay idle. The application threads use CPUs 16-27 and 72-83 on node 0, the NUMA node of the port. The other processes of the host use CPUs 0, 28-56 and 84-111. The client mix ran on a host with `isolcpus=1,2`, on 2 to 7 lcores (1-7 in item 6.2).

The CPU numbers are for the test host: 112 CPUs, node 0 is 0-27 and 56-83, and the HT sibling of CPU n is n + 56. Read the values of your host with `lscpu -e=CPU,NODE,CORE` and change the CPU lists to match.

### 6.1. Turn off automatic NUMA balancing

With `kernel.numa_balancing=1` the kernel scans the address space of each process about once per second, as task work on one of its threads. For MTL that thread is often a busy-polling scheduler, and every tasklet of that scheduler waits until the scan ends, for 32 us to 1.4 ms.

main sets `MPOL_LOCAL` on the scheduler threads, and v26.01 does not. v26.01 also takes 30 to 61 NUMA hinting faults per minute on the scheduler threads. Worst tasklet call in 60 s (Ice Lake with the fence of 6.3, Granite Rapids without it). In brackets: the NUMA scans and the hinting faults on the `mtl_sch_*` threads. A dash means not measured:

host | build | `numa_balancing=1` | `numa_balancing=0`
--- | --- | --- | ---
Ice Lake | main | 206 us (66 scans) | -
Ice Lake | v26.01 | 748 us (29 scans, 61 faults) | below 200 us (0 scans, 0 faults)
Granite Rapids | main | 869 us (68 scans) | -
Granite Rapids | v26.01 | 1.38 ms (46 scans, 37 faults) | 344 us and 370 us (0 scans, 0 faults)

```bash
sysctl -w kernel.numa_balancing=0
# keep it after a reboot
echo "kernel.numa_balancing=0" > /etc/sysctl.d/99-mtl-numa-balancing.conf
```

[Run Guide section 7.2](run.md#72-disable-automatic-numa-balancing) gives the same setting.

### 6.2. Start the application on a CPU that is not an lcore

`mtl_init()` binds the calling thread to the NUMA node of the port. When that thread runs on the other node, the kernel moves it to a CPU of the port node, and it can select an isolated CPU that is also an lcore.

The kernel never load-balances a task off an isolated CPU, and the threads created after that (`mtl_admin`, `mtl_stat` and the frame threads of the application) start on the same CPU. The admin thread then spins on a session lock that the preempted scheduler holds, for one full time slice.

Started on the lcore of `mtl_sch_0`, the worst tasklet call was 2.5 to 13 ms in 6 of 6 runs, and audio and ANC of that scheduler arrived up to 12.6 ms late. Started on CPU 8 of the same node, outside the lcore list of these runs (1-7), and with the host processes fenced off the lcores as in 6.3, main had no tasklet call above 200 us in 3 of 3 runs, and v26.01 had 1 to 2 calls of about 300 us.

Start the process on a CPU of the port node that is not in the lcore list. Then `numa_bind()` moves no thread, and no thread starts on an lcore. With the fence of 6.3, start the process in the scope that 6.3 shows:

```bash
# lcores 1-15 on NUMA node 0
taskset -c 16 ./app --lcores 1-15 ...
```

### 6.3. Keep other processes off the lcores and the application CPUs

`isolcpus` keeps the load balancer away from a CPU, but it does not move a task that starts on that CPU or inherits it. In a 60 s run of the client mix with `isolcpus=1,2`, one interactive process of the host alone switched in 5300 times (and 41500 times in a second run) on the two lcores.

The application threads that feed MTL need the same protection as the lcores: an ANC producer thread that shared its CPU with other processes woke 8.8 ms late and put its frame 6.8 ms after the media instant. MTL sent that frame 14 us after the put.

A cgroup cpuset, the fence, moves the processes of the host to the other CPUs at runtime. The MTL application gets a scope of its own: a process in a fenced slice cannot use the application CPUs or the lcores, and then `taskset` and `rte_eal_init()` fail. The fence needs cgroup v2 (`stat -fc %T /sys/fs/cgroup` gives `cgroup2fs`) and systemd 244 or later.

Set the fence only when no other MTL or DPDK process runs on the host. The fence changes the CPUs of all processes in these slices at once.
A thread that is pinned to an lcore moves to the house CPUs, and a DPDK process that starts in these slices after that stops in `rte_eal_init()`, because it cannot pin its lcores. Before you remove a fence, make sure that you set it: `systemctl show -p AllowedCPUs user.slice` shows the CPUs of the fence.

```bash
all=0-$(($(nproc --all) - 1))
house=0,28-56,84-111   # all CPUs minus the lcores, their HT siblings and the application CPUs
for s in user.slice system.slice init.scope; do
  systemctl set-property --runtime "$s" AllowedCPUs="$house"
done
systemd-run --scope --slice=mtl.slice -p AllowedCPUs="$all" -- taskset -c 16 ./app ...
```

A cpuset moves processes, not kernel threads. To keep device interrupts, timers, unbound kworkers and RCU callbacks off the lcores too, put the lcores and their HT siblings in the kernel boot parameters ([CPU Isolation Guide](isolation.md)): `isolcpus=managed_irq,domain,1-15,57-71 nohz_full=1-15,57-71 rcu_nocbs=1-15,57-71 irqaffinity=0,16-56,72-111`.

`managed_irq` moves only the managed interrupts. `irqaffinity` sends the other interrupts, for example the queues of the ice PF, to the other CPUs. The per-CPU threads (`ksoftirqd/N`, `kworker/N`) stay, but get less work. Do not put the application CPUs in `isolcpus`: the `domain` flag stops the load balancer, and the application threads then stay on the CPU where they started.

To remove the fence, set `AllowedCPUs` to all CPUs, then remove the runtime drop-in of that property. Do not use `systemctl revert`: it also removes the drop-ins that an administrator put in `/etc/systemd/system/<unit>.d/` and `/etc/systemd/system.control/`.

```bash
all=0-$(($(nproc --all) - 1))
for s in user.slice system.slice init.scope; do
  systemctl set-property --runtime "$s" AllowedCPUs="$all"
  rm -f "/run/systemd/system.control/$s.d/50-AllowedCPUs.conf"
done
systemctl daemon-reload
```

### 6.4. Turn off deep C-states on the application CPUs

The application threads sleep between two frames. A consumer in a blocking `st20p_rx_get_frame()` (`ST20P_RX_FLAG_BLOCK_GET`) sleeps for one frame period, and a producer sleeps until the time to put its next frame. A CPU with nothing to do for one frame period (16.7 ms at 59.94 fps) enters its deepest idle state, and the next wake-up pays the exit latency of that state.

On the Ice Lake host C6 has an exit latency of 170 us (`/sys/devices/system/cpu/cpu*/cpuidle/state*/latency`), and the `menu` governor selects it when it expects at least 600 us of idle time.

The consumer wake-up is the time from the `notify_frame_available` callback to the return of `st20p_rx_get_frame()`. The values below are with C6 on and with C6 off on the application CPUs only, 2 interleaved 60 s runs each (full mix, all session types):

metric | C6 on | C6 off
--- | --- | ---
consumer wake-up, p50 | 11 to 14 us | 10 to 11 us
consumer wake-up, p99 | 121 to 147 us | 14 to 20 us
consumer wake-up, p99.9 | 170 to 221 us | 25 to 31 us
frames with a consumer wake-up of 50 us or more | 1.7 to 7.6 % | 0.0 %
producer wake-up delay, p99.9 | 101 to 103 us | 63 to 64 us
2160p create to get, p99.9 | 18.82 ms | 18.65 to 18.66 ms

The frame complete times, the jitter and the RTP timestamps did not change: the schedulers busy-poll on their lcores and never enter an idle state. Only the application threads see the exit latency.

```bash
# application CPUs 16-27 and 72-83: turn off each idle state with an exit latency above 10 us
for c in $(seq 16 27) $(seq 72 83); do
  for s in /sys/devices/system/cpu/cpu"$c"/cpuidle/state*; do
    [ -f "$s/latency" ] || continue
    [ "$(cat "$s/latency")" -gt 10 ] && echo 1 > "$s/disable"
  done
done
```

C1 and C1E (1 us and 4 us) stay on. Granite Rapids also has C6P (210 us), and the same loop turns it off. Write 0 to `disable` to turn a state on again.

For a limit on one CPU only, write it to `/sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us` (for example 10), and write 0 to remove it. A process can also hold `/dev/cpu_dma_latency` open with a limit, but that limit applies to every CPU of the host.

### 6.5. Set the session create order and the scheduler quota

MTL places a session on a scheduler when the session is created. An ANC session has no quota and goes to the first scheduler. An audio session has a small quota (the scheduler quota / 300 for TX, / 1000 for RX) and goes to the first scheduler with that much free quota. A video session goes to the first scheduler with free quota for its bandwidth, or MTL starts a new scheduler.

With the create order 2160p, 1080p, 720p, audio, ANC and the default quota, MTL put both 2160p, three 1080p and two 720p sessions on one scheduler, and the ANC sessions with them. The 2160p sessions then ran at 25 to 40 fps and were up to 6.9 s late, until the admin thread moved the video sessions one at a time, 3, 9 and 15 s after the start.

Create the 720p, audio and ANC sessions first and then the 2160p and 1080p sessions, and set the quota of a scheduler to one 2160p session:

```c
struct mtl_init_params p;
memset(&p, 0, sizeof(p));
/* ... */
/* MTL counts 10357 Mb/s for one 2160p59.94 10 bit 4:2:2 session: no 1080p or 720p
 * session fits beside it */
p.data_quota_mbs_per_sch = 11000;
```

Then each 2160p session has a scheduler of its own from the start. In the measured runs no video frame was late or lost.

Each scheduler needs an lcore of its own. With this quota, the full mix with TX and RX in one process used 8 schedulers on lcores 1-15. With fewer lcores, the session create fails (`no free sch`).

### 6.6. Increase the RX frame buffers of audio and ANC

A pipeline RX session holds `framebuff_cnt` frames for the application. A 1 ms st30p session fills 4 frames in 4 ms, so with `framebuff_cnt = 4` a consumer that was 4 ms late lost audio frames (`back-pressure: framebuff pool empty`). With 16 frames for st30p and 8 for st40p, the full mix lost no frame:

```c
struct st30p_rx_ops ops_rx_audio;
struct st40p_rx_ops ops_rx_anc;
/* ... */
ops_rx_audio.framebuff_cnt = 16; /* 1 ms frames: 16 ms for the application */
ops_rx_anc.framebuff_cnt = 8;
```
