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

Available probes:
```bash
provider sys {
  probe log_msg(int level, char* msg);
  /* attach to enable the tasklet_time_measure at runtime */
  probe tasklet_time_measure();
}
```

#### 2.1.1 log_msg USDT

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

#### 2.1.2 tasklet_time_measure USDT

IMTL provides a flag named `MTL_FLAG_TASKLET_TIME_MEASURE` which enables the time measurement tracing feature, as the tasklet loop time is critical to our polling mode design. When this feature is activated during the initialization routine, IMTL will report the tasklet execution information through the status dump thread.

Typically, this flag is disabled in a production system since the time measurement tracing logic may incur additional CPU overhead. However, the USDT probe offers alternative methods to activate tracing at any time. The time measurement tracing becomes active when IMTL detects that a tasklet_time_measure USDT probe is attached.

Usage: Execute the following sample command to enable the probe, replacing "RxTxApp" with the name of your application process:

```bash
sudo bpftrace -e 'usdt::sys:tasklet_time_measure { printf("%s", strftime("%H:%M:%S", nsecs)); }' -p $(pidof RxTxApp)
```

Then you can then monitor the tasklet execution time by reviewing the relevant log entries.

```bash
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

#### 2.1.3 sessions_time_measure USDT

Usage: Execute the following sample command to enable the probe, replacing "RxTxApp" with the name of your application process:

```bash
sudo bpftrace -e 'usdt::sys:sessions_time_measure { printf("%s", strftime("%H:%M:%S", nsecs)); }' -p $(pidof RxTxApp)
```

Then you can then monitor the sessions tasklet execution time by reviewing the relevant log entries.

```bash
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

### 2.2 PTP tracing

Available probes:

```bash
provider ptp {
  probe ptp_msg(int port, int stage, uint64_t value);
  probe ptp_result(int port, int64_t delta, int64_t correct);
}
```

#### 2.2.1 ptp_msg USDT

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

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::ptp:ptp_result { printf("%s p%d,delta:%d,correct_delta:%d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
11:00:45 p0,delta:24622,correct_delta:470
11:00:45 p0,delta:25426,correct_delta:633
```

### 2.3 st20 tracing

Available probes:
```bash
provider st20 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
}
```

#### 2.3.1 tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p, tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
15:49:13 m0,s0: next frame 0(addr:0x3206b0e600, tmstamp:2464858558)
15:49:13 m0,s0: next frame 1(addr:0x320710e600, tmstamp:2464860060)
15:49:13 m0,s0: next frame 0(addr:0x3206b0e600, tmstamp:2464861561)
15:49:13 m0,s0: next frame 1(addr:0x320710e600, tmstamp:2464863063)
```

#### 2.3.2 tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
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

#### 2.3.3 rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:rx_frame_available { printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339234963, data size:5184000)
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339236465, data size:5184000)
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339237966, data size:5184000)
09:42:25 m1,s0: available frame 0(addr:0x3209f0e600, tmstamp:2339239468, data size:5184000)
```

#### 2.3.4 rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st20:rx_frame_put { printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
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

#### 2.3.5 rx_no_framebuffer USDT

Usage: This utility is designed to detect the absence of available frame buffers when a new timestamp is reached, typically indicating that the application has failed to return the frame in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st20:rx_no_framebuffer { printf("%s m%d,s%d: no framebuffer for tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

### 2.4 st30 tracing

Available probes:
```bash
provider st30 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
}
```

#### 2.4.1 tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
11:13:38 m0,s0: next frame 0(addr:0x3202400100)
11:13:38 m0,s0: next frame 1(addr:0x3207e07e80)
11:13:38 m0,s0: next frame 0(addr:0x3202400100)
11:13:38 m0,s0: next frame 1(addr:0x3207e07e80)
```

#### 2.4.2 tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
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

#### 2.4.3 rx_frame_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:rx_frame_available { printf("%s m%d,s%d: available frame %d(addr:%p, tmstamp:%u, data size:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715200, data size:192)
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715248, data size:192)
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715296, data size:192)
13:07:46 m0,s0: available frame 0(addr:0x320b60dbc0, tmstamp:3077715344, data size:192)
```

#### 2.4.4 rx_frame_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st30:rx_frame_put { printf("%s m%d,s%d: put frame %d(addr:%p)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
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

#### 2.4.5 rx_no_framebuffer USDT

Usage: This utility is designed to detect the absence of available frame buffers when a new timestamp is reached, typically indicating that the application has failed to return the frame in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st30:rx_no_framebuffer { printf("%s m%d,s%d: no framebuffer for tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

### 2.5 st40 tracing

Available probes:
```bash
provider st40 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t meta_num, int total_udw);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_mbuf_available(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp, uint32_t data_size);
  probe rx_mbuf_put(int m_idx, int s_idx, void* mbuf);
}
```

#### 2.5.1 tx_frame_next USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:tx_frame_next { printf("%s m%d,s%d: next frame %d(addr:%p), meta:%u udw:%d\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4, arg5); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
13:33:04 m0,s0: next frame 0(addr:0x3207e01c40), meta:1 udw:114
13:33:04 m0,s0: next frame 1(addr:0x3207e01a40), meta:1 udw:114
13:33:04 m0,s0: next frame 0(addr:0x3207e01c40), meta:1 udw:114
13:33:04 m0,s0: next frame 1(addr:0x3207e01a40), meta:1 udw:114
```

#### 2.5.2 tx_frame_done USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:tx_frame_done { printf("%s m%d,s%d: done frame %d(tmstamp:%u)\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
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

#### 2.5.3 rx_mbuf_available USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:rx_mbuf_available { printf("%s m%d,s%d: available mbuf:%p, tmstamp:%u, data size:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3, arg4); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
14:01:35 m0,s0: available mbuf:0x3207786e80, tmstamp:2840073508, data size:194
14:01:35 m0,s0: available mbuf:0x32077866c0, tmstamp:2840075010, data size:194
14:01:35 m0,s0: available mbuf:0x3207785f00, tmstamp:2840076511, data size:194
14:01:35 m0,s0: available mbuf:0x3207785740, tmstamp:2840078013, data size:194
```

#### 2.5.4 rx_mbuf_put USDT

usage: customize the application process name as your setup

```bash
sudo bpftrace -e 'usdt::st40:rx_mbuf_put { printf("%s m%d,s%d: put mbuf:%p\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:

```bash
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

#### 2.5.5 rx_mbuf_enqueue_fail USDT

Usage: This utility is designed to detect the application has failed to return the mbuf in a timely manner. Please customize the process name of the application according to your setup.

```bash
sudo bpftrace -e 'usdt::st40:rx_mbuf_enqueue_fail { printf("%s m%d,s%d: mbuf %p enqueue fail, tmstamp:%u\n", strftime("%H:%M:%S", nsecs), arg0, arg1, arg2, arg3); }' -p $(pidof RxTxApp)
```
