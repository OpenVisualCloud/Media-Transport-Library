# USDT

In eBPF, User Statically-Defined Tracing (USDT) probes bring the flexibility of kernel trace points to user-space applications. MTL provide USDT support by the SystemTap's API and the collection of DTRACE_PROBE() macros to help troubleshoot your applications in production with minimal runtime overhead.

## 1. Build

Prerequisites: install systemtap-sdt-dev.

For Ubuntu:
```bash
sudo apt-get install systemtap-sdt-dev
```

For CentOS:
```bash
sudo yum install systemtap-sdt-devel
```

Then rebuild MTL:

```bash
rm build
./build.sh
```

Check the build log and below build messages indicate the USDT support is enabled successfully.
```text
Program dtrace found: YES (/usr/bin/dtrace)
Has header "sys/sdt.h" : YES
Message: usdt tools check ok, build with USDT support
```

Then please find all USDT probes available in MTL by the `bpftrace` tool, the `bpftrace` installation please follow <https://github.com/bpftrace/bpftrace/blob/master/INSTALL.md>.

```bash
# customize the so path as your setup
sudo bpftrace -l 'usdt:/usr/local/lib/x86_64-linux-gnu/libmtl.so:*'
```

The example output:
```text
usdt:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp:ptp_msg
usdt:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp:ptp_result
```

Or by trace-bpfcc: customize the so path as your setup
```bash
tplist-bpfcc -l /usr/local/lib/x86_64-linux-gnu/libmtl.so -v
```

## 2. Tracing

All USDT probes is defined in [mt_usdt_provider.d](../lib/src/mt_usdt_provider.d)

### 2.1. sys tracing

Available probes:
```c
provider sys {
  /* attach to enable the usdt log msg at runtime */
  probe log_msg(int level, char* msg);
  /* attach to enable the tasklet_time_measure at runtime */
  probe tasklet_time_measure();
  /* attach to enable the sessions_time_measure at runtime */
  probe sessions_time_measure();
  /* attach to enable the pcap dump for cni rx queue */
  probe cni_pcap_dump(int port, char* dump_file, uint32_t pkts);
}
```

#### 2.1.1. log_msg USDT

The `log_msg` USDT is strategically positioned within the `MT_LOG` macro, enabling it to trace all log messages within MTL. It operates independently from the MTL Logging system, offering a means to monitor the system's status in production, where typically, the `enum mtl_log_level` is configured to `MTL_LOG_LEVEL_ERR`.

usage: customize the application process name as your setup

```bash
sudo BPFTRACE_STRLEN=128 bpftrace -e 'usdt::sys:log_msg { printf("%s l%d: %s", strftime("%H:%M:%S", nsecs), arg0, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
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

#### 2.1.2. tasklet_time_measure USDT

MTL provides a flag named `MTL_FLAG_TASKLET_TIME_MEASURE` which enables the time measurement tracing feature, as the tasklet loop time is critical to our polling mode design. When this feature is activated during the initialization routine, MTL will report the tasklet execution information through the status dump thread.

Typically, this flag is disabled in a production system since the time measurement tracing logic may incur additional CPU overhead. However, the USDT probe offers alternative methods to activate tracing at any time. The time measurement tracing becomes active when MTL detects that a tasklet_time_measure USDT probe is attached.

Usage: Execute the following sample command to enable the probe, replacing "RxTxApp" with the name of your application process:

```bash
sudo bpftrace -e 'usdt::sys:tasklet_time_measure { printf("%s", strftime("%H:%M:%S", nsecs)); }' -p $(pidof RxTxApp)
```

Then you can then monitor the tasklet execution time by reviewing the relevant log entries.

```text
MTL: 2024-03-26 16:00:19, * *    M T    D E V   S T A T E   * *
MTL: 2024-03-26 16:00:19, DEV(0): Avr rate, tx: 2612.232158 Mb/s, rx: 0.000000 Mb/s, pkts, tx: 2476211, rx: 0
MTL: 2024-03-26 16:00:19, DEV(1): Avr rate, tx: 0.000000 Mb/s, rx: 2604.192667 Mb/s, pkts, tx: 0, rx: 2476103
MTL: 2024-03-26 16:00:19, SCH(0:sch_0): tasklets 9, lcore 29(t_pid: 338795), avg loop 465 ns
MTL: 2024-03-26 16:00:19, SCH(0): time avg 0.46us max 115.02us min 0.41us
MTL: 2024-03-26 16:00:19, SCH(0,0): tasklet cni, avg 0.02us max 89.46us min 0.02us
MTL: 2024-03-26 16:00:19, SCH(0,1): tasklet tx_video_sessions_mgr, avg 0.04us max 49.32us min 0.03us
MTL: 2024-03-26 16:00:19, SCH(0,2): tasklet video_transmitter, avg 0.08us max 54.36us min 0.02us
MTL: 2024-03-26 16:00:19, SCH(0,3): tasklet tx_audio_sessions, avg 0.05us max 67.64us min 0.03us
MTL: 2024-03-26 16:00:19, SCH(0,4): tasklet audio_transmitter, avg 0.01us max 34.32us min 0.01us
MTL: 2024-03-26 16:00:19, SCH(0,5): tasklet tx_ancillary_sessions_mgr, avg 0.05us max 51.70us min 0.03us
MTL: 2024-03-26 16:00:19, SCH(0,6): tasklet ancillary_transmitter, avg 0.01us max 32.79us min 0.01us
MTL: 2024-03-26 16:00:19, SCH(0,7): tasklet rx_audio_sessions_mgr, avg 0.05us max 114.62us min 0.03us
MTL: 2024-03-26 16:00:19, SCH(0,8): tasklet rx_anc_sessions_mgr, avg 0.04us max 41.12us min 0.03us
MTL: 2024-03-26 16:00:19, SCH(1:sch_1): tasklets 1, lcore 30(t_pid: 338796), avg loop 88 ns
MTL: 2024-03-26 16:00:19, SCH(1): time avg 0.08us max 18.59us min 0.06us
MTL: 2024-03-26 16:00:19, SCH(1,0): tasklet rvs_pkt_rx, avg 0.05us max 18.54us min 0.03us
```

And if you want to trace both tasklet_time_measure and log_msg, follow below bpftrace script.

```bash
sudo BPFTRACE_STRLEN=128 bpftrace -e '
usdt::sys:tasklet_time_measure {
  printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:log_msg {
  printf("%s l%d: %s", strftime("%H:%M:%S", nsecs), arg0, str(arg1));
}
' -p $(pidof RxTxApp)
```

#### 2.1.3. sessions_time_measure USDT

Usage: Execute the following sample command to enable the probe, replacing "RxTxApp" with the name of your application process:

```bash
sudo bpftrace -e 'usdt::sys:sessions_time_measure { printf("%s", strftime("%H:%M:%S", nsecs)); }' -p $(pidof RxTxApp)
```

Then you can then monitor the sessions tasklet execution time by reviewing the relevant log entries.

```text
MTL: 2024-03-27 10:53:59, TX_VIDEO_SESSION(0,0:app_tx_video_0): fps 59.999419, frame 600 pkts 2467064:2466461 inflight 147929:148040
MTL: 2024-03-27 10:53:59, TX_VIDEO_SESSION(0,0): throughput 2611.029471 Mb/s: 0.000000 Mb/s, cpu busy 2.395878
MTL: 2024-03-27 10:53:59, TX_VIDEO_SESSION(0,0): tasklet time avg 0.01us max 56.88us min 0.00us
MTL: 2024-03-27 10:53:59, TX_AUDIO_SESSION(0,0:app_tx_audio0): fps 999.990357 frame cnt 10000, pkt cnt 10000, inflight count 0: 0
MTL: 2024-03-27 10:53:59, TX_AUDIO_SESSION(0,0): tasklet time avg 0.02us max 33.55us min 0.02us
MTL: 2024-03-27 10:53:59, TX_AUDIO_SESSION(0,0): tx delta avg 0.21us max 13.46us min 0.00us
MTL: 2024-03-27 10:53:59, TX_AUDIO_SESSION(0,0): get next frame max 11us, notify done max 0us
MTL: 2024-03-27 10:53:59, TX_AUDIO_MGR(0), pkts burst 10000
MTL: 2024-03-27 10:53:59, TX_ANC_SESSION(0:app_tx_ancillary0): fps 59.999409 frame cnt 600, pkt cnt 600
MTL: 2024-03-27 10:53:59, TX_ANC_SESSION(0): tasklet time avg 0.02us max 51.84us min 0.02us
MTL: 2024-03-27 10:53:59, TX_ANC_MGR, pkts burst 600
MTL: 2024-03-27 10:53:59, RX_VIDEO_SESSION(1,0:app_rx_video_0): fps 59.999408 frames 600 pkts 2466455
MTL: 2024-03-27 10:53:59, RX_VIDEO_SESSION(1,0:app_rx_video_0): throughput 2611.048395 Mb/s, cpu busy 0.625198
MTL: 2024-03-27 10:53:59, RX_VIDEO_SESSION(1,0): succ burst max 91, avg 1.036065
MTL: 2024-03-27 10:53:59, RX_VIDEO_SESSION(1,0): tasklet time avg 0.02us max 374.35us min 0.01us
MTL: 2024-03-27 10:53:59, RX_AUDIO_SESSION(0,0:app_rx_audio0): fps 999.989986, st30 received frames 10000, received pkts 10000
MTL: 2024-03-27 10:53:59, RX_AUDIO_SESSION(0,0): tasklet time avg 0.01us max 65.37us min 0.01us
MTL: 2024-03-27 10:53:59, RX_AUDIO_SESSION(0,0): notify frame max 10us
MTL: 2024-03-27 10:53:59, RX_ANC_SESSION(0:app_rx_anc0): fps 59.999337, st40 received frames 600, received pkts 600
MTL: 2024-03-27 10:53:59, RX_ANC_SESSION(0): tasklet time avg 0.01us max 59.42us min 0.01us
```

And if you want to trace both sessions_time_measure and log_msg, follow below bpftrace script.

```bash
sudo BPFTRACE_STRLEN=128 bpftrace -e '
usdt::sys:sessions_time_measure {
  printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:log_msg {
  printf("%s l%d: %s", strftime("%H:%M:%S", nsecs), arg0, str(arg1));
}
' -p $(pidof RxTxApp)
```

#### 2.1.4. cni_pcap_dump USDT

Usage: This utility is designed to capture and store cni packets received over the network to a standard pcap file for offline analyses. Please note the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::sys:cni_pcap_dump { printf("%s p%d: dumped pcap file %s pkts %u\n", strftime("%H:%M:%S", nsecs), arg0, str(arg1), arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
16:10:07 p0: dumped pcap file cni_p0_10000_G8iOd6.pcapng pkts 81
16:10:07 p1: dumped pcap file cni_p1_10000_Grf8CU.pcapng pkts 0
16:10:17 p0: dumped pcap file cni_p0_10000_G8iOd6.pcapng pkts 206
16:10:17 p1: dumped pcap file cni_p1_10000_Grf8CU.pcapng pkts 0
```

### 2.2. PTP tracing

Available probes:

```c
provider ptp {
  probe ptp_msg(int port, int stage, uint64_t value);
  probe ptp_result(int port, int64_t delta, int64_t correct);
}
```

#### 2.2.1. ptp_msg USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::ptp:ptp_msg { printf("%s p%u,t%u:%llu\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
11:00:37 p0,t2:1711077318712839835
11:00:37 p0,t1:1711077318712861878
11:00:37 p0,t3:1711077318719139980
11:00:37 p0,t4:1711077318719167902
```

#### 2.2.2. ptp_result USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::ptp:ptp_result { printf("%s p%d,delta:%d,correct_delta:%d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
11:00:45 p0,delta:24622,correct_delta:470
11:00:45 p0,delta:25426,correct_delta:633
```

### 2.3. st20 tracing

Available probes:
```c
provider st20 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
  /* attach to enable the pcap dump at runtime */
  probe rx_pcap_dump(int m_idx, int s_idx, int s_port, char* dump_file, uint32_t pkts);
  /* incomplete frame */
  probe rx_frame_incomplete(int m_idx, int s_idx, int f_idx, uint32_t tmstamp, uint32_t data_size, uint32_t expect_size);
}
```

#### 2.3.1. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p, tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:49:13 m0,s0: next frame 0(addr:0x3206b0e600, tmstamp:2464858558)
15:49:13 m0,s0: next frame 1(addr:0x320710e600, tmstamp:2464860060)
15:49:13 m0,s0: next frame 0(addr:0x3206b0e600, tmstamp:2464861561)
15:49:13 m0,s0: next frame 1(addr:0x320710e600, tmstamp:2464863063)
```

#### 2.3.2. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:49:43 m0,s0: done frame 0(tmstamp:2467616814)
15:49:43 m0,s0: done frame 1(tmstamp:2467618315)
15:49:44 m0,s0: done frame 0(tmstamp:2467619817)
15:49:44 m0,s0: done frame 1(tmstamp:2467621318)
```

And if you want to trace both tx_frame_next and tx_frame_done, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st20:tx_frame_next {
  printf("%s m%d,s%d: next frame %d(addr:%p, tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4);
}
usdt::st20:tx_frame_done {
  printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.3.3. rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:rx_frame_available { printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339234963, data size:5184000)
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339236465, data size:5184000)
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339237966, data size:5184000)
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339239468, data size:5184000)
```

#### 2.3.4. rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:rx_frame_put { printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
10:42:08 m1,s0: put frame 0(addr:0x3209f0e600)
10:42:08 m1,s0: put frame 0(addr:0x3209f0e600)
10:42:08 m1,s0: put frame 0(addr:0x3209f0e600)
10:42:08 m1,s0: put frame 0(addr:0x3209f0e600)
```

And if you want to trace both rx_frame_available and rx_frame_put, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st20:rx_frame_available {
  printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5);
}
usdt::st20:rx_frame_put {
  printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.3.5. rx_no_framebuffer USDT

Usage: This utility is designed to detect the absence of available frame buffers when a new timestamp is reached, typically indicating that the application has failed to return the frame in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st20:rx_no_framebuffer { printf("%s m%d,s%d: no framebuffer for tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

#### 2.3.6. tx_frame_dump USDT

Usage: This utility is designed to capture and store video frames transmitted over the network. Attaching to this hook initiates the process, which continues to dump frames to a local file every 5 seconds until detachment occurs. Please note, the video file is large and the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::st20:tx_frame_dump { printf("%s m%d,s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg3, arg4, str(arg2)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:55:15 m0,s0: dump frame 0x320850e600 size 5184000 to imtl_usdt_st20tx_m0s0_1920_1080_5T6CWy.yuv
15:55:20 m0,s0: dump frame 0x3207f0e600 size 5184000 to imtl_usdt_st20tx_m0s0_1920_1080_uI7c3W.yuv
```

#### 2.3.7. rx_frame_dump USDT

Usage: This utility is designed to capture and store video frames received over the network. Attaching to this hook initiates the process, which continues to dump frames to a local file every 5 seconds until detachment occurs. Please note, the video file is large and the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::st20:rx_frame_dump { printf("%s m%d,s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg3, arg4, str(arg2)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:57:35 m1,s0: dump frame 0x320bb0e600 size 5184000 to imtl_usdt_st20rx_m1s0_1920_1080_MEFuOW.yuv
15:57:40 m1,s0: dump frame 0x320bb0e600 size 5184000 to imtl_usdt_st20rx_m1s0_1920_1080_ehmjPp.yuv
```

#### 2.3.8. rx_pcap_dump USDT

Usage: This utility is designed to capture and store st20 packets received over the network to a standard pcap file for offline analyses. Attaching to this hook initiates the process, which will dump the packets of 5 frames. Please note the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::st20:rx_pcap_dump { printf("%s m%d,s%dp%d: dumped pcap file %s pkts %u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, str(arg3), arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:53:27 m1,s0p0: dumped pcap file st22rx_s0p0_20570_C9ErTF.pcapng pkts 20570
```

#### 2.3.9. rx_frame_incomplete USDT

This tracking point is engineered to detect any instances of packet loss and failures in constructing a complete frame by MTL. The underlying causes can vary widely, for example, the sender might not transmit all pixels, packets could be lost due to switch issues, or the NIC might discard packets if the receiver's queue is full.

```bash
sudo bpftrace -e 'usdt::st20:rx_frame_incomplete { printf("%s m%d,s%d: incomplete frame %d(tmstamp:%u, recv size:%u, expect full size: %u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below if MTL failed to constructed one full frame:

```text
11:05:54 m0,s0: incomplete frame 1(tmstamp:1167274660, recv size:2891700, expect full size: 5184000)
11:06:45 m0,s0: incomplete frame 1(tmstamp:1172316697, recv size:4956840, expect full size: 5184000)
```

### 2.4. st30 tracing

Available probes:
```c
provider st30 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int m_idx, int s_idx, char* dump_file, int frames);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int m_idx, int s_idx, char* dump_file, int frames);
}
```

#### 2.4.1. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
11:13:38 m0,s0: next frame 0(addr:0x3202400100)
11:13:38 m0,s0: next frame 1(addr:0x3207e07e80)
11:13:38 m0,s0: next frame 0(addr:0x3202400100)
11:13:38 m0,s0: next frame 1(addr:0x3207e07e80)
```

#### 2.4.2. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
11:14:06 m0,s0: done frame 0(tmstamp:2750348096)
11:14:06 m0,s0: done frame 1(tmstamp:2750348144)
11:14:06 m0,s0: done frame 0(tmstamp:2750348192)
11:14:06 m0,s0: done frame 1(tmstamp:2750348240)
```

And if you want to trace both tx_frame_next and tx_frame_done, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st30:tx_frame_next {
  printf("%s m%d,s%d: next frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
usdt::st30:tx_frame_done {
  printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.4.3. rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:rx_frame_available { printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715200, data size:192)
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715248, data size:192)
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715296, data size:192)
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715344, data size:192)
```

#### 2.4.4. rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:rx_frame_put { printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:08:11 m0,s0: put frame 0(addr:0x320b60dbc0)
13:08:11 m0,s0: put frame 0(addr:0x320b60dbc0)
13:08:11 m0,s0: put frame 0(addr:0x320b60dbc0)
13:08:11 m0,s0: put frame 0(addr:0x320b60dbc0)
```

And if you want to trace both rx_frame_available and rx_frame_put, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st30:rx_frame_available {
  printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5);
}
usdt::st30:rx_frame_put {
  printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.4.5. rx_no_framebuffer USDT

Usage: This utility is designed to detect the absence of available frame buffers when a new timestamp is reached, typically indicating that the application has failed to return the frame in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st30:rx_no_framebuffer { printf("%s m%d,s%d: no framebuffer for tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

#### 2.4.6. tx_frame_dump USDT

Usage: This utility is designed to capture and store audio frames transmitted over the network. Attaching to this hook initiates the process, which continues to dump frames to a local file until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st30:tx_frame_dump { printf("%s m%d,s%d: dump %d frames to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg3, str(arg2)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
17:17:35 m0,s0: dump 1000 frames to imtl_usdt_st30tx_m0s0_48000_24_c2_eNlkQk.pcm
17:17:36 m0,s0: dump 2000 frames to imtl_usdt_st30tx_m0s0_48000_24_c2_eNlkQk.pcm
17:17:37 m0,s0: dump 3000 frames to imtl_usdt_st30tx_m0s0_48000_24_c2_eNlkQk.pcm
```

Then use ffmpeg tools to convert ram PCM file to a wav, customize the format as your setup.

```bash
ffmpeg -f s24be -ar 48000 -ac 2 -i imtl_usdt_st30tx_m0s0_48000_24_c2_eNlkQk.pcm dump.wav
```

#### 2.4.7. rx_frame_dump USDT

Usage: Similar to tx_frame_dump hook, this utility is designed to capture and store audio frames received over the network. Attaching to this hook initiates the process, which continues to dump frames to a local file until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st30:rx_frame_dump { printf("%s m%d,s%d: dump %d frames to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg3, str(arg2)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
10:26:07 m0,s0: dump 1000 frames to imtl_usdt_st30rx_m0s0_48000_24_c2_qeITcK.pcm
10:26:08 m0,s0: dump 2000 frames to imtl_usdt_st30rx_m0s0_48000_24_c2_qeITcK.pcm
10:26:09 m0,s0: dump 3000 frames to imtl_usdt_st30rx_m0s0_48000_24_c2_qeITcK.pcm
```

Then use ffmpeg tools to convert ram PCM file to a wav, customize the format as your setup.

```bash
ffmpeg -f s24be -ar 48000 -ac 2 -i imtl_usdt_st30rx_m0s0_48000_24_c2_qeITcK.pcm dump.wav
```

#### 2.4.8. rx_pcap_dump USDT

Usage: This utility is designed to capture and store st30 packets received over the network to a standard pcap file for offline analyses. Attaching to this hook initiates the process, which will dump the packets of 5 seconds. Please note the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::st30:rx_pcap_dump { printf("%s m%d,s%dp%d: dumped pcap file %s pkts %u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, str(arg3), arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:46:02 m0,s0p0: dumped pcap file st30rx_s0p0_5000_FxX2r5.pcapng pkts 5000
```

### 2.5. st40 tracing

Available probes:
```c
provider st40 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t meta_num, int total_udw);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_mbuf_available(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp, uint32_t data_size);
  probe rx_mbuf_enqueue_fail(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp);
  probe rx_mbuf_put(int m_idx, int s_idx, void* mbuf);
}
```

#### 2.5.1. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p), meta:%u udw:%d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:33:04 m0,s0: next frame 0(addr:0x3207e01c40), meta:1 udw:114
13:33:04 m0,s0: next frame 1(addr:0x3207e01a40), meta:1 udw:114
13:33:04 m0,s0: next frame 0(addr:0x3207e01c40), meta:1 udw:114
13:33:04 m0,s0: next frame 1(addr:0x3207e01a40), meta:1 udw:114
```

#### 2.5.2. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:34:41 m0,s0: done frame 0(tmstamp:2694872452)
13:34:41 m0,s0: done frame 1(tmstamp:2694873954)
13:34:41 m0,s0: done frame 0(tmstamp:2694875455)
13:34:41 m0,s0: done frame 1(tmstamp:2694876957)
```

And if you want to trace both tx_frame_next and tx_frame_done, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st40:tx_frame_next {
  printf("%s m%d,s%d: next frame %d(addr:%p), meta:%u udw:%d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5);
}
usdt::st40:tx_frame_done {
  printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.5.3. rx_mbuf_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:rx_mbuf_available { printf("%s m%d,s%d: available mbuf:%p, tmstamp:%u, data size:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:01:35 m0,s0: available mbuf:0x3207786e80, tmstamp:2840073508, data size:194
14:01:35 m0,s0: available mbuf:0x32077866c0, tmstamp:2840075010, data size:194
14:01:35 m0,s0: available mbuf:0x3207785f00, tmstamp:2840076511, data size:194
14:01:35 m0,s0: available mbuf:0x3207785740, tmstamp:2840078013, data size:194
```

#### 2.5.4. rx_mbuf_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:rx_mbuf_put { printf("%s m%d,s%d: put mbuf:%p\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:03:15 m0,s0: put mbuf:0x3207882cc0
14:03:15 m0,s0: put mbuf:0x3207882500
14:03:15 m0,s0: put mbuf:0x3207881d40
```

And if you want to trace both rx_frame_available and rx_frame_put, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st40:rx_mbuf_available {
  printf("%s m%d,s%d: available mbuf:%p, tmstamp:%u, data size:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4);
}
usdt::st40:rx_mbuf_put {
  printf("%s m%d,s%d: put mbuf:%p\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2);
}
' -p $(pidof RxTxApp)
```

#### 2.5.5. rx_mbuf_enqueue_fail USDT

Usage: This utility is designed to detect the application has failed to return the mbuf in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st40:rx_mbuf_enqueue_fail { printf("%s m%d,s%d: mbuf %p enqueue fail, tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

#### 2.5.6. st40p pipeline tracing

The ST40 pipeline helpers (used by `RxSt40PipelineSample`, `TxSt40PipelineSample`, or any app built on `st40_pipeline_api.h`) expose their own provider to focus on frame-level callbacks across both TX and RX paths:

```c
provider st40p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  probe tx_frame_drop(int idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_frame_available(int idx, int f_idx, uint32_t meta_num);
  probe rx_frame_get(int idx, int f_idx, uint32_t meta_num);
  probe rx_frame_put(int idx, int f_idx, uint32_t meta_num);
  /* attach to enable on-demand dumps */
  probe rx_frame_dump(int idx, char* dump_file, uint32_t meta_num, uint32_t bytes);
}
```

To watch for ancillary frames that arrive too late to flush, hook the TX drop probe (customize the process name for your setup):

```bash
sudo bpftrace -e 'usdt::st40p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof TxSt40PipelineSample)
```

Example output like below:

```text
09:33:12 s0: drop frame 0(timestamp:241037184)
09:33:12 s0: drop frame 1(timestamp:241037728)
```

Typical RX-side traces (customize the process name/PID for your sample or RxTxApp process):

```bash
sudo BPFTRACE_STRLEN=256 bpftrace -e '
usdt::st40p:rx_frame_available {
  printf("%s rx%u: frame %u ready (meta=%u)\n",
         strftime("%H:%M:%S", nsecs), arg0, arg1, arg2);
}
usdt::st40p:rx_frame_get {
  printf("%s rx%u: user claimed frame %u\n",
         strftime("%H:%M:%S", nsecs), arg0, arg1);
}
usdt::st40p:rx_frame_put {
  printf("%s rx%u: frame %u returned (meta=%u)\n",
         strftime("%H:%M:%S", nsecs), arg0, arg1, arg2);
}
usdt::st40p:rx_frame_dump {
  printf("%s rx%u: dump meta=%u bytes=%u -> %s\n",
         strftime("%H:%M:%S", nsecs), arg0, arg2, arg3, str(arg1));
}
' -p $(pidof RxSt40PipelineSample)
```

`rx_frame_dump` fires only while you are attached; `arg2` is the metadata entry count and
`arg3` is the number of payload bytes flushed to the generated file (for example
`imtl_usdt_st40prx_s0_447_SzG5Yv.bin`). Attach `usdt::sys:log_msg` alongside these probes if
you want the scheduler/tasklet summaries without reconfiguring the sample log level. If
`bpftrace` reports `couldn't get argument N`, rebuild/install `libmtl.so` with
`-Denable_usdt=true` so the refreshed probe metadata (including the extra RX meta argument and
the renamed dump byte field) is visible system-wide.

### 2.6. st20p tracing

Available probes:
```c
provider st20p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va, int stat);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_get(int idx, int f_idx, void* va);
  probe rx_frame_put(int idx, int f_idx, void* va);
  probe rx_frame_available(int idx, int f_idx, uint32_t tmstamp);
  probe rx_frame_dump(int idx, char* dump_file, uint32_t data_size);
}
```

#### 2.6.1. tx_frame_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:tx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:20:25 s0: get frame 0(addr:0x3205f0e600)
09:20:25 s0: get frame 1(addr:0x320650e600)
09:20:25 s0: get frame 0(addr:0x3205f0e600)
09:20:25 s0: get frame 1(addr:0x320650e600)
```

#### 2.6.2. tx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:tx_frame_put { printf("%s s%d: put frame %d(addr:%p,stat:%d)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:21:10 s0: put frame 0(addr:0x3205f0e600,stat:3)
09:21:10 s0: put frame 1(addr:0x320650e600,stat:3)
09:21:10 s0: put frame 0(addr:0x3205f0e600,stat:3)
09:21:10 s0: put frame 1(addr:0x320650e600,stat:3)
```

#### 2.6.3. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:tx_frame_next { printf("%s s%d: next frame %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:21:55 s0: next frame 0
09:21:55 s0: next frame 1
09:21:55 s0: next frame 0
09:21:55 s0: next frame 1
```

#### 2.6.4. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:tx_frame_done { printf("%s s%d: done frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:22:58 s0: done frame 0(timestamp:3639347793)
09:22:58 s0: done frame 1(timestamp:3639349294)
09:22:58 s0: done frame 0(timestamp:3639350796)
09:22:58 s0: done frame 1(timestamp:3639352297)
```

#### 2.6.5. tx_frame_drop USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:25:04 s0: drop frame 0(timestamp:3639471120)
09:25:04 s0: drop frame 1(timestamp:3639472621)
```

And if you want to trace all st20p tx events, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st20p:tx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st20p:tx_frame_put { printf("%s s%d: put frame %d(addr:%p,stat:%d)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }
usdt::st20p:tx_frame_done { printf("%s s%d: done frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st20p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st20p:tx_frame_next { printf("%s s%d: next frame %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1); }
' -p $(pidof RxTxApp)
```

#### 2.6.6. rx_frame_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:rx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
10:37:16 s0: get frame 0(addr:0x3208a17000)
10:37:16 s0: get frame 1(addr:0x3209217000)
10:37:16 s0: get frame 2(addr:0x3209a17000)
```

#### 2.6.7. rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:rx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
10:37:33 s0: put frame 0(addr:0x3208a17000)
10:37:33 s0: put frame 1(addr:0x3209217000)
10:37:33 s0: put frame 2(addr:0x3209a17000)
```

#### 2.6.8. rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20p:rx_frame_available { printf("%s s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
10:37:57 s0: available frame 0(addr:0x320a30e600, tmstamp:4044186727, data size:5184000)
10:37:57 s0: available frame 1(addr:0x320a90e600, tmstamp:4044188229, data size:5184000)
10:37:57 s0: available frame 2(addr:0x320a30e600, tmstamp:4044189730, data size:5184000)
```

And if you want to trace all st20p rx events, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st20p:rx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st20p:rx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st20p:rx_frame_available { printf("%s s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }
' -p $(pidof RxTxApp)
```

#### 2.6.9. tx_frame_dump USDT

Usage: Attaching to this hook initiates the process, which continues to dump frames to a local file every 5 seconds until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st20p:tx_frame_dump { printf("%s s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg2, arg3, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:26:41 s0: dump frame 0x3205a17000 size 8294400 to imtl_usdt_st20ptx_s0_1920_1080_I0y9V0.yuv
13:26:46 s0: dump frame 0x3206217000 size 8294400 to imtl_usdt_st20ptx_s0_1920_1080_CzYDQe.yuv
```

#### 2.6.10. rx_frame_dump USDT

Usage: Attaching to this hook initiates the process, which continues to dump frames to a local file every 5 seconds until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st20p:rx_frame_dump { printf("%s s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg2, arg3, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:27:24 s0: dump frame 0x3209a17000 size 8294400 to imtl_usdt_st20prx_s0_1920_1080_rKtpGn.yuv
13:27:29 s0: dump frame 0x3209217000 size 8294400 to imtl_usdt_st20prx_s0_1920_1080_UKWN27.yuv
```

### 2.7. st22 tracing

Available probes:
```c
provider st22 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
}
```

#### 2.7.1. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p, tmstamp:%u, size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
10:08:04 m0,s0: next frame 0(addr:0x32042400c0, tmstamp:3519515195, size:777600)
10:08:04 m0,s0: next frame 1(addr:0x3203a400c0, tmstamp:3519516696, size:777600)
10:08:04 m0,s0: next frame 0(addr:0x32042400c0, tmstamp:3519518198, size:777600)
10:08:04 m0,s0: next frame 1(addr:0x3203a400c0, tmstamp:3519519699, size:777600)
```

#### 2.7.2. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
13:57:31 m0,s0: done frame 0(tmstamp:12993706)
13:57:31 m0,s0: done frame 1(tmstamp:12995207)
13:57:31 m0,s0: done frame 0(tmstamp:12996709)
13:57:31 m0,s0: done frame 1(tmstamp:12998210)
```

And if you want to trace both tx_frame_next and tx_frame_done, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st22:tx_frame_next {
  printf("%s m%d,s%d: next frame %d(addr:%p, tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4);
}
usdt::st22:tx_frame_done {
  printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.7.3. rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22:rx_frame_available { printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:20:13 m1,s0: available frame 0(addr:0x320890e600, tmstamp:135541631, data size:777600)
14:20:13 m1,s0: available frame 1(addr:0x3208f0e600, tmstamp:135543133, data size:777600)
14:20:13 m1,s0: available frame 0(addr:0x320890e600, tmstamp:135544634, data size:777600)
14:20:13 m1,s0: available frame 1(addr:0x3208f0e600, tmstamp:135546136, data size:777600)
```

#### 2.7.4. rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22:rx_frame_put { printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:20:43 m1,s0: put frame 0(addr:0x320890e600)
14:20:43 m1,s0: put frame 1(addr:0x3208f0e600)
14:20:43 m1,s0: put frame 0(addr:0x320890e600)
14:20:43 m1,s0: put frame 1(addr:0x3208f0e600)
```

And if you want to trace both rx_frame_available and rx_frame_put, follow below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st22:rx_frame_available {
  printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5);
}
usdt::st22:rx_frame_put {
  printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3);
}
' -p $(pidof RxTxApp)
```

#### 2.7.5. rx_no_framebuffer USDT

Usage: This utility is designed to detect the absence of available frame buffers when a new timestamp is reached, typically indicating that the application has failed to return the frame in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st22:rx_no_framebuffer { printf("%s m%d,s%d: no framebuffer for tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

#### 2.7.6. tx_frame_dump USDT

Usage: This utility is designed to capture and store st22 codestream transmitted over the network. Attaching to this hook initiates the process, which continues to dump codestream to a local file every 5 seconds until detachment occurs. Please note, the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::st22:tx_frame_dump { printf("%s m%d,s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg3, arg4, str(arg2)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:12:14 m0,s0: dump frame 0x3203a400c0 size 777660 to imtl_usdt_st22tx_m0s0_1920_1080_OP9MbJ.raw
14:12:19 m0,s0: dump frame 0x32032400c0 size 777660 to imtl_usdt_st22tx_m0s0_1920_1080_rAt7U8.raw
```

#### 2.7.7. rx_frame_dump USDT

Usage: This utility is designed to capture and store st22 codestream received over the network. Attaching to this hook initiates the process, which continues to dump codestream to a local file every 5 seconds until detachment occurs. Please note, the dump happens on the tasklet path which may affect the performance.

```bash
sudo bpftrace -e 'usdt::st22:rx_frame_dump { printf("%s m%d,s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg3, arg4, str(arg2)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
14:26:18 m1,s0: dump frame 0x3208f0e600 size 777600 to imtl_usdt_st22rx_m1s0_1920_1080_ctwWDR.raw
14:26:23 m1,s0: dump frame 0x320890e600 size 777600 to imtl_usdt_st22rx_m1s0_1920_1080_G5EWrj.raw
```

### 2.8. st22p tracing

Available probes:
```c
provider st22p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_get(int idx, int f_idx, void* va);
  probe rx_frame_put(int idx, int f_idx, void* va);
  probe rx_frame_available(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int idx, char* dump_file, uint32_t data_size);
}
```

#### 2.8.1. tx_frame_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:01:15 s0: get frame 0(addr:0x3205b0e600)
15:01:15 s0: get frame 1(addr:0x320610e600)
15:01:15 s0: get frame 0(addr:0x3205b0e600)
15:01:15 s0: get frame 1(addr:0x320610e600)
```

#### 2.8.2. tx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_frame_put { printf("%s s%d: put frame %d(addr:%p,stat:%d,size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:02:34 s0: put frame 0(addr:0x3205b0e600)
15:02:34 s0: put frame 1(addr:0x320610e600)
15:02:34 s0: put frame 0(addr:0x3205b0e600)
15:02:34 s0: put frame 1(addr:0x320610e600)
```

#### 2.8.3. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_frame_next { printf("%s s%d: next frame %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:04:37 s0: next frame 0
15:04:37 s0: next frame 1
15:04:37 s0: next frame 0
15:04:37 s0: next frame 1
```

#### 2.8.4. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_frame_done { printf("%s s%d: done frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:05:38 s0: done frame 0(timestamp:380829674)
15:05:38 s0: done frame 1(timestamp:380831176)
15:05:38 s0: done frame 0(timestamp:380832677)
15:05:38 s0: done frame 1(timestamp:380834179)
```

#### 2.8.5. tx_frame_drop USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:06:42 s0: drop frame 0(timestamp:380846682)
15:06:42 s0: drop frame 1(timestamp:380848184)
```

And if you want to trace all st22p tx events, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st22p:tx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st22p:tx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st22p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st22p:tx_frame_done { printf("%s s%d: done frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st22p:tx_frame_next { printf("%s s%d: next frame %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1); }
' -p $(pidof RxTxApp)
```

#### 2.8.6. rx_frame_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:rx_frame_get { printf("%s s%d: get frame %d(addr:%p,size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:10:54 s0: get frame 0(addr:0x320770e600)
15:10:54 s0: get frame 1(addr:0x3207d0e600)
15:10:54 s0: get frame 2(addr:0x320830e600)
```

#### 2.8.7. rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:rx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:11:08 s0: put frame 0(addr:0x320770e600)
15:11:08 s0: put frame 1(addr:0x3207d0e600)
15:11:08 s0: put frame 2(addr:0x320830e600)
```

#### 2.8.8. rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:rx_frame_available { printf("%s s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:11:24 s0: available frame 0(addr:0x320890e600, tmstamp:411970784, data size:777600)
15:11:24 s0: available frame 1(addr:0x3208f0e600, tmstamp:411972286, data size:777600)
15:11:24 s0: available frame 2(addr:0x320890e600, tmstamp:411973787, data size:777600)
```

And if you want to trace all st22p rx events, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st22p:rx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st22p:rx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st22p:rx_frame_available { printf("%s s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }
' -p $(pidof RxTxApp)
```

#### 2.8.9. tx_frame_dump USDT

Usage: Attaching to this hook initiates the process, which continues to dump frames to a local file every 5 seconds until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st22p:tx_frame_dump { printf("%s s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg2, arg3, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:21:59 s0: dump frame 0x320610e600 size 5184000 to imtl_usdt_st22ptx_s0_1920_1080_2XVMPQ.yuv
15:22:04 s0: dump frame 0x3205b0e600 size 5184000 to imtl_usdt_st22ptx_s0_1920_1080_oOJwjO.yuv
```

#### 2.8.10. rx_frame_dump USDT

Usage: Attaching to this hook initiates the process, which continues to dump frames to a local file every 5 seconds until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st22p:rx_frame_dump { printf("%s s%d: dump frame %p size %u to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg2, arg3, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:22:59 s0: dump frame 0x320830e600 size 5184000 to imtl_usdt_st22prx_s0_1920_1080_gwSetx.yuv
15:23:04 s0: dump frame 0x3207d0e600 size 5184000 to imtl_usdt_st22prx_s0_1920_1080_N72BCd.yuv
```

#### 2.8.11. tx_encode_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_encode_get { printf("%s s%d: get encode %d(src:%p,dst:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
16:20:25 s0: get encode 0(src:0x3205b0e600,dst:0x3203a400fc)
16:20:25 s0: get encode 1(src:0x320610e600,dst:0x32032400fc)
```

#### 2.8.12. tx_encode_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:tx_encode_put { printf("%s s%d: put encode %d(src:%p,dst:%p), result: %d, codestream size: %u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
16:20:45 s0: put encode 0(src:0x3205b0e600,dst:0x3203a400fc), result: 0, codestream size: 777600
16:20:45 s0: put encode 1(src:0x320610e600,dst:0x32032400fc), result: 0, codestream size: 777600
```

And if you want to trace both tx_encode_get and tx_encode_put, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st22p:tx_encode_get { printf("%s s%d: get encode %d(src:%p,dst:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }
usdt::st22p:tx_encode_put { printf("%s s%d: put encode %d(src:%p,dst:%p), result %d, codestream size: %u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }
' -p $(pidof RxTxApp)
```

#### 2.8.13. rx_decode_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:rx_decode_get { printf("%s s%d: get decode %d(src:%p,dst:%p), codestream size: %u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
16:18:43 s0: get decode 0(src:0x3208f0e600,dst:0x320770e600), codestream size: 777600
16:18:43 s0: get decode 1(src:0x320890e600,dst:0x3207d0e600), codestream size: 777600
16:18:43 s0: get decode 2(src:0x3208f0e600,dst:0x320830e600), codestream size: 777600
```

#### 2.8.14. rx_decode_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st22p:rx_decode_put { printf("%s s%d: put decode %d(src:%p,dst:%p), result: %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
16:22:03 s0: put decode 0(src:0x320890e600,dst:0x320770e600), result: 0
16:22:03 s0: put decode 1(src:0x3208f0e600,dst:0x3207d0e600), result: 0
16:22:03 s0: put decode 2(src:0x320890e600,dst:0x320830e600), result: 0
```

And if you want to trace both rx_decode_get and rx_decode_put, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st22p:rx_decode_get { printf("%s s%d: get decode %d(src:%p,dst:%p), codestream size: %u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }
usdt::st22p:rx_decode_put { printf("%s s%d: put decode %d(src:%p,dst:%p), result: %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }
' -p $(pidof RxTxApp)
```

### 2.9. st30p tracing

Available probes:
```c
provider st30p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int idx, char* dump_file, int frames);
  /* rx */
  probe rx_frame_get(int idx, int f_idx, void* va);
  probe rx_frame_put(int idx, int f_idx, void* va);
  probe rx_frame_available(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int idx, char* dump_file, int frames);
}
```

#### 2.9.1. tx_frame_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:tx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:50:45 s0: get frame 0(addr:0x3203405500)
15:50:45 s0: get frame 1(addr:0x3203404940)
15:50:45 s0: get frame 2(addr:0x3203403d80)
```

#### 2.9.2. tx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:tx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:51:27 s0: put frame 0(addr:0x3203405500)
15:51:27 s0: put frame 1(addr:0x3203404940)
15:51:27 s0: put frame 2(addr:0x3203403d80)
```

#### 2.9.3. tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:tx_frame_next { printf("%s s%d: next frame %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:51:45 s0: next frame 0
15:51:45 s0: next frame 1
15:51:45 s0: next frame 2
```

#### 2.9.4. tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:tx_frame_done { printf("%s s%d: done frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:51:58 s0: done frame 0(timestamp:447475136)
15:51:58 s0: done frame 1(timestamp:447475616)
15:51:58 s0: done frame 2(timestamp:447476096)
```

#### 2.9.5. tx_frame_drop USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:52:11 s0: drop frame 0(timestamp:447476608)
15:52:11 s0: drop frame 1(timestamp:447477088)
```

And if you want to trace all st30p tx events, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st30p:tx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st30p:tx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st30p:tx_frame_drop { printf("%s s%d: drop frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st30p:tx_frame_done { printf("%s s%d: done frame %d(timestamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st30p:tx_frame_next { printf("%s s%d: next frame %d\n", strftime("%H:%M:%S", nsecs), arg0, arg1); }
' -p $(pidof RxTxApp)
```

#### 2.9.6. rx_frame_get USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:rx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:57:50 s0: get frame 0(addr:0x3203402140)
15:57:50 s0: get frame 1(addr:0x3203401580)
15:57:50 s0: get frame 2(addr:0x32034009c0)
```

#### 2.9.7. rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:rx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:58:14 s0: put frame 0(addr:0x3203402140)
15:58:14 s0: put frame 1(addr:0x3203401580)
15:58:14 s0: put frame 2(addr:0x32034009c0)
```

#### 2.9.8. rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30p:rx_frame_available { printf("%s s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
15:59:45 s0: available frame 0(addr:0x32034009c0, tmstamp:469935200, data size:2880)
15:59:46 s0: available frame 1(addr:0x3203401580, tmstamp:469935728, data size:2880)
15:59:46 s0: available frame 2(addr:0x3203402140, tmstamp:469936208, data size:2880)
```

And if you want to trace all st30p rx events, use below bpftrace script.

```bash
sudo bpftrace -e '
usdt::st30p:rx_frame_get { printf("%s s%d: get frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st30p:rx_frame_put { printf("%s s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }
usdt::st30p:rx_frame_available { printf("%s s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }
' -p $(pidof RxTxApp)
```

#### 2.9.9. tx_frame_dump USDT

Usage: This utility is designed to capture and store audio frames transmitted over the network. Attaching to this hook initiates the process, which continues to dump frames to a local file until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st30p:tx_frame_dump { printf("%s s%d: dump %d frames to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg2, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
16:22:22 s0: dump 100 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:23 s0: dump 200 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:24 s0: dump 300 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:25 s0: dump 400 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:26 s0: dump 500 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:27 s0: dump 600 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:28 s0: dump 700 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:29 s0: dump 800 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
16:22:30 s0: dump 900 frames to imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm
```

Then use ffmpeg tools to convert ram PCM file to a wav, customize the format as your setup.

```bash
ffmpeg -f s24be -ar 48000 -ac 2 -i imtl_usdt_st30ptx_s0_48000_24_c2_LgAKKR.pcm dump.wav
```

#### 2.9.10. rx_frame_dump USDT

Usage: Similar to tx_frame_dump hook, this utility is designed to capture and store audio frames received over the network. Attaching to this hook initiates the process, which continues to dump frames to a local file until detachment occurs.

```bash
sudo bpftrace -e 'usdt::st30p:rx_frame_dump { printf("%s s%d: dump %d frames to %s\n", strftime("%H:%M:%S", nsecs), arg0, arg2, str(arg1)); }' -p $(pidof RxTxApp)
```

Example output like below:

```text
09:00:34 s0: dump 100 frames to imtl_usdt_st30prx_s0_48000_24_c2_X0ZwK2.pcm
09:00:35 s0: dump 200 frames to imtl_usdt_st30prx_s0_48000_24_c2_X0ZwK2.pcm
09:00:36 s0: dump 300 frames to imtl_usdt_st30prx_s0_48000_24_c2_X0ZwK2.pcm
```

Then use ffmpeg tools to convert ram PCM file to a wav, customize the format as your setup.

```bash
ffmpeg -f s24be -ar 48000 -ac 2 -i imtl_usdt_st30prx_s0_48000_24_c2_X0ZwK2.pcm dump.wav
```
