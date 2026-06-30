# Run Guide for FreeBSD

This guide provides instructions for running Media Transport Library (MTL) applications on FreeBSD after successful build.

## Prerequisites

- MTL and DPDK built and installed (see [Build Guide](./build_FREEBSD.md))
- Hugepages configured and contigmem loaded
- Intel E810/E830 NIC for hardware rate limiting (or other DPDK-compatible NIC)

## 1. System Configuration

### 1.1. Verify Hugepages

```bash
# Check contigmem is loaded
kldstat | grep contigmem

# Check configuration
sysctl hw.contigmem.num_buffers
sysctl hw.contigmem.buffer_size

# Expected: 1024 buffers of 2MB each
```

### 1.2. Check NIC Status

```bash
# List network interfaces
ifconfig

# Check PCI devices
pciconf -lv | grep -A3 "Ethernet"

# For DPDK PMD: NIC should be detached from kernel
# For kernel socket: NIC should be attached (ice0, etc.)
```

### 1.3. CPU Isolation (Optional, Recommended for Low Latency)

Use `cpuset` to bind MTL processes to specific cores:

```bash
# Example: Reserve cores 2-7 for MTL
# Run MTL with cpuset
cpuset -l 2-7 ./RxTxApp ...

# Or set in MTL via lcores parameter (DPDK EAL)
./RxTxApp --p_port 0000:af:00.0 --lcores '2-7' ...
```

## 2. Running RxTxApp

### 2.1. Basic Loopback Test (DPDK PMD)

Test with two ports in loopback configuration:

```bash
cd $mtl_source_code

# Find PCI addresses of your NICs
pciconf -lv | grep -B3 "Ethernet"

# Example: pci0:175:0:0 and pci0:175:0:1

# Run loopback test (1080p59 video)
sudo ./build/tests/tools/RxTxApp/RxTxApp \
  --p_port 0000:af:00.0 \
  --r_port 0000:af:00.1 \
  --config_file tests/tools/RxTxApp/loop_json/1080p59.json \
  --log_level info

# Expected: TX and RX statistics, zero packet loss
```

### 2.2. Using Kernel Socket Backend

For testing without DPDK PMD:

```bash
# Ensure NIC is attached to kernel driver
ifconfig ice0  # Should show interface

# Run with kernel socket backend
sudo ./build/tests/tools/RxTxApp/RxTxApp \
  --p_port kernel:ice0 \
  --r_port kernel:ice1 \
  --config_file tests/tools/RxTxApp/loop_json/1080p59.json \
  --log_level info
```

**Note:** Kernel socket backend has lower performance but is easier for testing.

### 2.3. JSON Configuration

Sample JSON files are in `tests/tools/RxTxApp/loop_json/`:

```bash
# List available configurations
ls tests/tools/RxTxApp/loop_json/

# Common formats:
# - 1080p59.json (1080p @ 59.94 fps)
# - 1080p60.json (1080p @ 60 fps)
# - 4k60.json (4K @ 60 fps)
```

## 3. DPDK EAL Parameters

### 3.1. Common DPDK Flags

MTL uses DPDK EAL. Key parameters:

```bash
# Core assignment: -l <core_list>
--lcores '0-3'

# Hugepages: -n <num_channels>
-n 4

# PCI device: -a <pci_address>
-a 0000:af:00.0

# Example combined:
sudo ./RxTxApp \
  --lcores '2-5' \
  -n 4 \
  -a 0000:af:00.0 \
  -a 0000:af:00.1 \
  --p_port 0000:af:00.0 \
  --r_port 0000:af:00.1 \
  --config_file loop_json/1080p60.json
```

### 3.2. In-Memory Mode (Recommended)

Use `--in-memory` to avoid persistent hugepage files:

```bash
sudo ./RxTxApp \
  --in-memory \
  --lcores '2-5' \
  -a 0000:af:00.0 \
  --p_port 0000:af:00.0 \
  --config_file tx_1080p60.json
```

## 4. PTP Clock Synchronization

For ST2110 timing compliance, PTP synchronization is critical.

### 4.1. Install ptpd2

```bash
# Install PTP daemon
pkg install ptpd2

# Run ptpd2 on NIC interface
sudo ptpd2 -i ice0 -M -V

# Monitor PTP status
# Check for "Offset from master" convergence
```

### 4.2. Verify PTP in MTL

MTL logs PTP synchronization status:

```bash
# Look for PTP messages in MTL output
# Expected: "PTP: offset <X> ns, delay <Y> ns"
# Good sync: offset < 100ns
```

## 5. Performance Tuning

### 5.1. FreeBSD Sysctl Tuning

```bash
# Increase network buffer sizes
sudo sysctl net.inet.tcp.recvspace=4194304
sudo sysctl net.inet.tcp.sendspace=4194304

# Increase interface queue length
sudo sysctl net.isr.maxthreads=4

# Set CPU to performance mode (if supported)
sudo sysctl dev.cpu.0.freq=$(sysctl -n dev.cpu.0.freq_levels | awk '{print $1}')

# Disable interrupt moderation for DPDK polling
sudo sysctl dev.ice.0.rx_itr=0
sudo sysctl dev.ice.0.tx_itr=0
```

### 5.2. MTL Scheduler Tuning

Adjust scheduler quota based on system performance:

```bash
# Default: ~10 Gbps per scheduler core
# Increase for higher density:
./RxTxApp --data_quota_mbs_per_sch 12000 ...

# Enable session migration for dynamic load balancing:
./RxTxApp --tx_video_migrate --rx_video_migrate ...
```

### 5.3. Enable Sleep Mode (Lower CPU Usage)

For non-latency-critical workloads:

```bash
# Enable tasklet sleep to reduce CPU to ~50%
./RxTxApp --tasklet_sleep ...
```

**Trade-off:** Increases latency, may not meet ST2110 narrow pacing.

## 6. Monitoring and Debugging

### 6.1. DTrace for Tracing

MTL supports USDT probes. Use DTrace on FreeBSD:

```bash
# List available probes
sudo dtrace -l | grep mtl

# Example: Trace TX frame events
sudo dtrace -n 'mtl*:::tx_frame_done { printf("%s", copyinstr(arg0)); }'

# Trace packet timing
sudo dtrace -n 'mtl*:::pacing { printf("pacing: %d ns", arg1); }'
```

### 6.2. Performance Monitoring

```bash
# Monitor CPU usage per core
top -P

# Network statistics
netstat -I ice0 -w 1

# PMC (Performance Monitoring Counters)
sudo pmcstat -S instructions -w 1
```

### 6.3. Common Issues

#### No Packets Received

1. Check multicast routing:
   ```bash
   netstat -rn | grep 224
   ```

2. Verify flow director rules (DPDK PMD only):
   ```bash
   # MTL logs flow rule installation
   # Look for "flow rule created" in output
   ```

3. Check NIC promiscuous mode (if needed):
   ```bash
   ifconfig ice0 promisc
   ```

#### High Packet Loss

1. Increase RX descriptors in JSON config:
   ```json
   "nb_rx_desc": 4096
   ```

2. Check CPU usage - scheduler may be overloaded:
   ```bash
   # Reduce sessions per core or add more cores
   ```

3. Verify hugepages not exhausted:
   ```bash
   sysctl hw.contigmem
   ```

## 7. Multi-Process Deployment (SR-IOV)

### 7.1. Enable SR-IOV on FreeBSD

```bash
# Check if NIC supports SR-IOV
pciconf -lc pci0:175:0:0 | grep SR-IOV

# Enable VFs (Virtual Functions)
sudo sysctl hw.ice.sriov_enabled=1

# Create VFs
sudo sysctl hw.ice.0.num_vfs=4

# List VFs
pciconf -lv | grep "Ethernet.*Virtual"
```

### 7.2. Assign VFs to MTL Instances

```bash
# Instance 1: Use VF 0
sudo ./RxTxApp -a 0000:af:00.2 --p_port 0000:af:00.2 ...

# Instance 2: Use VF 1
sudo ./RxTxApp -a 0000:af:00.3 --p_port 0000:af:00.3 ...
```

## 8. Sample Workflows

### 8.1. Video TX Test

```bash
# Transmit 1080p60 video to multicast group
sudo ./RxTxApp \
  --p_port 0000:af:00.0 \
  --config_file tx_1080p60.json \
  --lcores '2-3' \
  --log_level info
```

### 8.2. Video RX Test

```bash
# Receive from multicast group
sudo ./RxTxApp \
  --r_port 0000:af:00.1 \
  --config_file rx_1080p60.json \
  --lcores '4-5' \
  --log_level info
```

### 8.3. Bidirectional Test

```bash
# TX and RX simultaneously
sudo ./RxTxApp \
  --p_port 0000:af:00.0 \
  --r_port 0000:af:00.1 \
  --config_file loop_json/1080p60.json \
  --lcores '2-7' \
  --log_level info
```

## 9. Integration with Other Tools

### 9.1. FFmpeg Plugin

MTL provides FFmpeg plugin (if built):

```bash
# Use MTL as FFmpeg input/output
ffmpeg -f mtl_st2110 -i "st2110://239.0.0.1:5000" output.mp4
```

### 9.2. OBS Plugin

For live streaming with OBS (if OBS plugin built):

```bash
# Load MTL plugin in OBS
# Add "Media Transport Library" source
```

## 10. Troubleshooting

### 10.1. DPDK EAL Init Fails

```bash
# Error: "EAL: Cannot get hugepage information"
# Solution: Load contigmem module
sudo kldload contigmem

# Error: "EAL: Driver cannot attach device"
# Solution: Unbind NIC from kernel
sudo devctl detach pci0:175:0:0
```

### 10.2. Permission Denied

```bash
# Error: Opening contigmem failed
# Solution: Check permissions or run as root
sudo ./RxTxApp ...

# Or add user to operator group
sudo pw groupmod operator -m $USER
```

### 10.3. Build Warnings

Check [Build Guide FAQ](./build_FREEBSD.md#7-faq) for build-related issues.

## Additional Resources

- [MTL Design Guide](./design.md)
- [DPDK FreeBSD Getting Started](https://doc.dpdk.org/guides/freebsd_gsg/)
- [FreeBSD Handbook: Advanced Networking](https://docs.freebsd.org/en/books/handbook/advanced-networking/)
- [ST2110 Compliance Documentation](./compliance.md)

## Support

For FreeBSD-specific issues:
- Check [GitHub Issues](https://github.com/OpenVisualCloud/Media-Transport-Library/issues)
- FreeBSD forums: [https://forums.freebsd.org](https://forums.freebsd.org)
