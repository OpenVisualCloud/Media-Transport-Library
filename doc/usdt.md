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

Or by trace-bpfcc:
```bash
# customize the so path as your setup
tplist-bpfcc -l /usr/local/lib/x86_64-linux-gnu/libmtl.so -v
```

## 2. Tracing

### 2.1 PTP tracing

#### 2.1.1 ptp_msg USDT

Provider: ptp, probe name: ptp_msg, parm1: port, parm2: stage, parm3: value

usage:
```bash
# customize the application process name as your setup
sudo bpftrace -e 'usdt::ptp:ptp_msg { printf("p%u,t%u:%llu\n", arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:
```bash
p0,t2:1711003484946427306
p0,t1:1711003484946449452
p0,t3:1711003484953729512
p0,t4:1711003484953757844
```

Or by trace-bpfcc:
```bash
#customize the so path and application process name as your setup
sudo trace-bpfcc 'u:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp_msg "p%u,t%u:%llu", arg1,arg2,arg3' -T -p $(pidof RxTxApp)
```

#### 2.1.2 ptp_result USDT

Provider: ptp, Name: ptp_result, parm1: port, parm2: raw delta, parm3: correct delta of PI.

usage:
```bash
# customize the application process name as your setup
sudo bpftrace -e 'usdt::ptp:ptp_result { printf("p%d,delta:%d,correct_delta:%d\n", arg0, arg1, arg2); }' -p $(pidof RxTxApp)
```

Example output like below:
```bash
p0,delta:25122,correct_delta:67
p0,delta:25216,correct_delta:353
```

Or by trace-bpfcc:
```bash
# customize the so path and application process name as your setup
sudo trace-bpfcc 'u:/usr/local/lib/x86_64-linux-gnu/libmtl.so:ptp_result "p%u,delta:%d,correct_delta:%d", arg1,arg2,arg3' -T -p $(pidof RxTxApp)
```
