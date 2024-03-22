# USDT

In eBPF, User Statically-Defined Tracing (USDT) probes bring the flexibility of kernel trace points to user-space applications. IMTL provide USDT support by the SystemTap's API and the collection of DTRACE_PROBE() macros to help troubleshoot your applications in production with minimal runtime overhead.

## 1. Build

Prerequisites: install systemtap-sdt-dev.

For Ubuntu:
```bash
sudo apt-get install systemtap-sdt-dev
```

For Centos:
```bash
sudo yum install systemtap-sdt-devel
```

Then rebuild IMTL:

```bash
rm build
./build.sh
```

Check the build log and below build messages indicate the USDT support is enabled successfully.
```bash
Program dtrace found: YES (/usr/bin/dtrace)
Has header "sys/sdt.h" : YES
Message: usdt tools check ok, build with USDT support
```

Then please find all USDT probes available in IMTL by the `bpftrace` tool, the `bpftrace` installation please follow <https://github.com/bpftrace/bpftrace/blob/master/INSTALL.md>.

```bash
# customize the so path as your setup
sudo bpftrace -l 'usdt:/usr/local/lib/x86_64-linux-gnu/libmtl.so:*'
```

The example output:
```bash
usdt:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp:ptp_msg
usdt:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp:ptp_result
```

Or by trace-bpfcc: customize the so path as your setup
```bash
tplist-bpfcc -l /usr/local/lib/x86_64-linux-gnu/libmtl.so -v
```

## 2. Tracing

All USDT probes is defined in [mt_usdt_provider.d](../lib/src/mt_usdt_provider.d)

### 2.1 sys tracing

#### 2.1.1 log_msg USDT

Provider: sys, probe name: log_msg, parm1: level, parm2: char* msg

The `log_msg` USDT is strategically positioned within the `MT_LOG` macro, enabling it to trace all log messages within IMTL. It operates independently from the IMTL Logging system, offering a means to monitor the system's status in production, where typically, the `enum mtl_log_level` is configured to `MTL_LOG_LEVEL_ERR`.

usage: customize the application process name as your setup
```bash
sudo BPFTRACE_STRLEN=128 bpftrace -e 'usdt::sys:log_msg { printf("%s l%d: %s", strftime("%H:%M:%S", nsecs), arg0, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:
```bash
10:58:51 l2: * *    M T    D E V   S T A T E   * *
10:58:51 l2: DEV(0): Avr rate, tx: 2610.318120 Mb/s, rx: 0.009552 Mb/s, pkts, tx: 2465803, rx: 127
10:58:51 l2: DEV(1): Avr rate, tx: 0.000000 Mb/s, rx: 2602.483541 Mb/s, pkts, tx: 0, rx: 2465819
10:58:51 l2: SCH(0:sch_0): tasklets 3, lcore 29(t_pid: 171515), avg loop 106 ns
10:58:51 l2: SCH(1:sch_1): tasklets 1, lcore 30(t_pid: 171516), avg loop 46 ns
10:58:51 l2: CNI(0): eth_rx_rate 0.009552 Mb/s, eth_rx_cnt 127
10:58:51 l2: CNI(1): eth_rx_rate 0.000552 Mb/s, eth_rx_cnt 2
10:58:51 l2: PTP(0): time 1711077212656358421, 2024-03-22 11:12:55
10:58:51 l2: PTP(0): delta avg 24799, min 24190, max 25343, cnt 40
10:58:51 l2: PTP(0): correct_delta avg 304, min -236, max 904, cnt 40
10:58:51 l2: PTP(0): path_delay avg 3011, min 2576, max 3586, cnt 40
10:58:51 l2: PTP(0): mode l4, sync cnt 40, expect avg 49607:243@0.250441s t2_t1_delta -21844 t4_t3_delta 27763
10:58:51 l2: PTP(0): t2_t1_delta_calibrate 1 t4_t3_delta_calibrate 1
10:58:51 l2: TX_VIDEO_SESSION(0,0:app_tx_video_0): fps 59.899444, frame 599 pkts 2467464:2466867 inflight 148002:148112
10:58:51 l2: TX_VIDEO_SESSION(0,0): throughput 2611.461089 Mb/s: 0.000000 Mb/s, cpu busy 0.687289
10:58:51 l2: RX_VIDEO_SESSION(1,0:app_rx_video_0): fps 59.899436 frames 599 pkts 2466858
10:58:51 l2: RX_VIDEO_SESSION(1,0:app_rx_video_0): throughput 2611.475818 Mb/s, cpu busy 0.393960
10:58:51 l2: RX_VIDEO_SESSION(1,0): succ burst max 21, avg 1.036176
10:58:51 l2: * *    E N D    S T A T E   * *
```

### 2.2 PTP tracing

#### 2.2.1 ptp_msg USDT

Provider: ptp, probe name: ptp_msg, parm1: port, parm2: stage, parm3: value

usage: customize the application process name as your setup
```bash
sudo bpftrace -e 'usdt::ptp:ptp_msg { printf("%s p%u,t%u:%llu\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:
```bash
11:00:37 p0,t2:1711077318712839835
11:00:37 p0,t1:1711077318712861878
11:00:37 p0,t3:1711077318719139980
11:00:37 p0,t4:1711077318719167902
```

#### 2.2.2 ptp_result USDT

Provider: ptp, Name: ptp_result, parm1: port, parm2: raw delta, parm3: correct delta of PI.

usage: customize the application process name as your setup
```bash
sudo bpftrace -e 'usdt::ptp:ptp_result { printf("%s p%d,delta:%d,correct_delta:%d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:
```bash
11:00:45 p0,delta:24622,correct_delta:470
11:00:45 p0,delta:25426,correct_delta:633
```
