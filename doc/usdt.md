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

Check the build log and below build message indicate the USDT support is enabled successfully.
```bash
Message: sys/sdt.h found, build with USDT support
```

Then please find all USDT probes available in IMTL by the bcc tool, the bcc installation please follow <https://github.com/iovisor/bcc/blob/master/INSTALL.md>.

```bash
# customize the so path as your setup
tplist-bpfcc -l /usr/local/lib/x86_64-linux-gnu/libmtl.so -v
```

The example output:
```bash
b'ptp':b'ptp_msg' [sema 0x0]
  5 location(s)
  3 argument(s)
b'ptp':b'ptp_result' [sema 0x0]
  1 location(s)
  3 argument(s)
```

## 2. Tracing

### 2.1 PTP tracing

#### 2.1.1 ptp_msg USDT

Provider: ptp, probe name: ptp_msg, parm1: port, parm2: stage, parm3: value

usage:
```bash
#customize the so path and application process name as your setup
sudo trace-bpfcc 'u:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp_msg "p%u,t%u:%u", arg1,arg2,arg3' -T -p $(pidof RxTxApp)
```

Example output like below:
```bash
14:15:57 57865   57896   mtl_sch_0       ptp_msg          p0,t2:2437966777
14:15:57 57865   57896   mtl_sch_0       ptp_msg          p1,t2:2437970801
14:15:57 57865   57896   mtl_sch_0       ptp_msg          p0,t1:2437988526
14:15:57 57865   57896   mtl_sch_0       ptp_msg          p1,t1:2437988526
14:15:57 57865   57867   dpdk-intr       ptp_msg          p0,t3:2440444696
14:15:57 57865   57867   dpdk-intr       ptp_msg          p1,t3:2440461242
14:15:57 57865   57896   mtl_sch_0       ptp_msg          p0,t4:2440472270
14:15:57 57865   57896   mtl_sch_0       ptp_msg          p1,t4:2440489246
```

#### 2.1.2 ptp_result USDT

Provider: ptp, Name: ptp_result, parm1: port, parm2: raw delta, parm3: correct delta of PI.

usage:
```bash
# customize the so path and application process name as your setup
sudo trace-bpfcc 'u:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp_result "p%u,delta:%d,correct_delta:%d", arg1,arg2,arg3' -T -p $(pidof RxTxApp)
```

Example output like below:
```bash
14:15:20 57865   57896   mtl_sch_0       ptp_result       p0,delta:25346,correct_delta:506
14:15:20 57865   57896   mtl_sch_0       ptp_result       p1,delta:25003,correct_delta:243
14:15:20 57865   57896   mtl_sch_0       ptp_result       p0,delta:24681,correct_delta:159
14:15:20 57865   57896   mtl_sch_0       ptp_result       p1,delta:24976,correct_delta:501
```
