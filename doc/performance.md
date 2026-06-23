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
