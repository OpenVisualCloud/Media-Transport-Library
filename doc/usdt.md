# USDT

In eBPF, User Statically-Defined Tracing (USDT) probes bring the flexibility of kernel trace points to user-space applications. IMTL provide USDT support by the SystemTap's API and the collection of DTRACE_PROBE() macros to help the troubleshoot your applications in production with minimal runtime overhead.

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
tplist-bpfcc -l /usr/local/lib/x86_64-linux-gnu/libmtl.so
```

The example output:
```bash
b'/usr/local/lib/x86_64-linux-gnu/libmtl.so' * b'ptp':b'ptp_msg'
b'/usr/local/lib/x86_64-linux-gnu/libmtl.so' b'ptp':b'ptp_result'
```

## 2. Tracing

### 2.1 PTP tracing

#### 2.1.1 ptp_msg USDT

Provider: ptp, probe name: ptp_msg, parm1: port, parm2: stage, parm3: value

usage:
```bash
#customize the so path and application process name as your setup
sudo trace-bpfcc 'u:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp_msg "p%u:t%u:%u", arg1,arg2,arg3' -T -p $(pidof RxTxApp)
```

Example output like below:
```bash
13:49:59 56551   56582   mtl_sch_0       ptp_msg          p0:t2:3290678345
13:49:59 56551   56582   mtl_sch_0       ptp_msg          p1:t2:3290681891
13:49:59 56551   56553   dpdk-intr       ptp_msg          p0:t3:3292260786
13:49:59 56551   56553   dpdk-intr       ptp_msg          p1:t3:3292277582
13:49:59 56551   56582   mtl_sch_0       ptp_msg          p0:t1:3290700443
13:49:59 56551   56582   mtl_sch_0       ptp_msg          p1:t1:3290700443
13:49:59 56551   56582   mtl_sch_0       ptp_msg          p0:t4:3292288427
13:49:59 56551   56582   mtl_sch_0       ptp_msg          p1:t4:3292305771
```

#### 2.1.2 ptp_result USDT

Provider: ptp, Name: ptp_result, parm1: port, parm2: raw delta, parm3: correct delta of PI.

usage:
```bash
# customize the so path and application process name as your setup
sudo trace-bpfcc 'u:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp_result "p%u:delta:%d:correct_delta:%d", arg1,arg2,arg3' -T -p $(pidof RxTxApp)
```

Example output like below:
```bash
13:50:29 56551   56582   mtl_sch_0       ptp_result       p1:delta:25179:correct_delta:536
13:50:29 56551   56582   mtl_sch_0       ptp_result       p0:delta:25026:correct_delta:-65
13:50:29 56551   56582   mtl_sch_0       ptp_result       p1:delta:25214:correct_delta:161
13:50:29 56551   56582   mtl_sch_0       ptp_result       p0:delta:24796:correct_delta:124
13:50:29 56551   56582   mtl_sch_0       ptp_result       p1:delta:25176:correct_delta:548
```
