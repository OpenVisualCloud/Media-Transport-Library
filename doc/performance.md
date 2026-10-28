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

## 6. RX header split

Without an offload, the RX video tasklet copies the payload of each packet from the mbuf into the frame. The copy reads about 1.2 KB and writes about 1.2 KB for each packet, and it fills the cache with lines that the next packets need.
DMA offload gives the copy to a DSA device, but the tasklet must submit the copy and poll for its completion. With header split, the NIC writes the payload of each packet directly into its slot in the frame, so the CPU does no payload copy and needs no DMA device.

![RX header split](png/hdr_split.svg)

See [Header Split Guide](experimental/header_split.md) for the use cases, the requirements and the frame layout.

### 6.1. Where header split helps

* Many RX video sessions on one core, when the payload copy limits the session count. The copy is the largest per packet cost of the CPU copy path, so a receiver at 4K or with many 1080p sessions gains most.
* A host that is bound by memory bandwidth, see section 3. Header split removes the read and the write of the copy, so each session moves less DDR traffic.
* A host without a DSA device, or with its DSA devices used for other work. Header split takes the copy off the CPU without a DMA device.
* An application that reads the frame in the transport format: for example a recorder or a forwarder of RFC 4175 data, or an application that reads the frame one packet at a time and can step over the slots.

### 6.2. Where header split does not help

* An application that needs a contiguous frame, or a `st20p` converter. The converters cannot read a slotted frame, and a copy out of the slots costs as much as the copy that header split removes.
* ST 2022-7 redundancy, a VF on iavf, shared RSS, a shared RX queue, AF_XDP or the kernel socket path. Header split needs a dedicated RX queue on an ice PF.
* A port that changes its rate limit while it receives. A rate limit change fails with `-EBUSY` while a header split session runs.
* A port with many plain RX sessions. When one RX queue of the port splits, the ice PMD uses the scalar RX path for all RX queues of the port, and the plain sessions can cost more.
* A sender whose GPM packet length is not the length that the RX session expects, or GPM_SL lines that end in a shorter packet.
* An application that holds a frame for longer than one lap of the slots. The NIC writes the slots again while the application reads them.

### 6.3. Measure the gain

Measure on the target platform before you select header split. `tests/tools/perf_testing` gives the RX cost for each packet from the work counters of the scheduler. Run it with `--time_measure` once for each RX path, with the same session count and format, and compare `ns_per_pkt` of the `rxcost` record and `work_avg_ns` of the `rvs_pkt_rx` tasklet:

```bash
# CPU copy
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:00.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:00.1 --t_sip 192.168.85.81 \
  --sessions 8 --time_measure --duration 60
# header split, the same load
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:00.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:00.1 --t_sip 192.168.85.81 \
  --sessions 8 --time_measure --duration 60 --hdr_split
```

The TX sessions of the same process can share the RX scheduler. At a high session count, send from a second process with `--tx_only`, as the [perf_testing guide](../tests/tools/perf_testing/README.md) says.

Then add sessions until the first incomplete frame for each path, to find the session count on one core. The CPU copy baseline changes with `enable_rx_video_nt_store` of section 5, so record that option with the result.
