# Run Guide for Intel I226-V (Ubuntu, 1G/2.5G)

This guide provides an Intel I226-V focused runtime flow for Media Transport Library (MTL) on Ubuntu.
It targets:

* Ubuntu hosts
* Intel I226-V link speeds of 1GbE or 2.5GbE
* Preferred backend order: **DPDK first**, with **AF_XDP fallback**

## 1. Host readiness checklist (Ubuntu)

### 1.1 Install required packages

```bash
sudo apt-get update
sudo apt-get install -y \
  git gcc meson ninja-build python3 python3-pyelftools pkg-config \
  libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev \
  systemtap-sdt-dev llvm clang flex byacc \
  linux-tools-common linux-tools-generic linux-tools-$(uname -r) \
  ethtool pciutils numactl jq \
  linuxptp
```

Do not use `python3 -m pip install --user pyelftools ninja` on recent Ubuntu
releases unless you are intentionally using a virtual environment. Ubuntu's
Python packages are externally managed, so `python3-pyelftools` and
`ninja-build` should be installed with `apt`.

### 1.2 BIOS/Kernel pre-checks

* Enable IOMMU in BIOS/UEFI.
* Keep power management conservative for media workloads (avoid deep C-states when possible).
* Verify that the kernel was booted with IOMMU enabled:

```bash
cat /proc/cmdline
```

Typical Intel platform options include `intel_iommu=on iommu=pt`.

### 1.3 Hugepages (for DPDK path)

```bash
# Example: reserve 1024 x 2MB pages
sudo sysctl -w vm.nr_hugepages=1024

# Verify
grep -E "HugePages_Total|HugePages_Free|Hugepagesize" /proc/meminfo
```

### 1.4 I226-V capability checks

Replace `<ifname>` with your I226-V Linux interface name.

```bash
lspci -nn | grep -i -E "ethernet|intel"
ethtool -i <ifname>
ethtool -k <ifname>
ethtool -T <ifname>
```

## 2. DPDK-first runtime flow (preferred)

### 2.1 Build/install

Use the I226-V build helper from [Build Guide](build.md):

```bash
cd $mtl_source_code
./script/build_i226v.sh
```

### 2.2 Prepare VFIO access

Follow the shared VFIO permissions flow in [Run Guide](run.md#31-allow-current-user-to-access-devvfio-devices).
Load the VFIO kernel modules before binding the I226-V to `vfio-pci`:

```bash
sudo modprobe vfio
sudo modprobe vfio-pci
lsmod | grep -E "^vfio|^vfio_pci"
```

If `dpdk-devbind.py` reports `Warning: no supported DPDK kernel modules are loaded`
or `Error: Driver 'vfio-pci' is not loaded`, re-run the `modprobe` commands above and
then retry the bind command.

### 2.3 Bind I226-V port to `vfio-pci`

```bash
# Identify BDF and current driver
sudo dpdk-devbind.py -s

# Ensure the DPDK kernel driver is loaded
sudo modprobe vfio-pci

# Bring interface down before rebinding
sudo ip link set <ifname> down

# Bind to vfio-pci (replace with your BDF)
sudo dpdk-devbind.py -b vfio-pci 0000:xx:yy.z

# Verify
sudo dpdk-devbind.py -s
```

### 2.4 Run MTL sample with DPDK backend

Set your JSON interface `name` to the DPDK BDF (for example `0000:xx:yy.z`) and run:

```bash
./tests/tools/RxTxApp/build/RxTxApp --config_file config/tx_1v.json
```

### 2.5 Run PTP with the DPDK IGC driver

When the I226-V is bound to `vfio-pci`, Linux `ptp4l` cannot access the port. Use the
MTL built-in PTP implementation instead:

```bash
./tests/tools/RxTxApp/build/RxTxApp \
  --config_file <config.json> \
  --test_time 120 \
  --log_level info \
  --log_file /tmp/i226_dpdk_ptp.log \
  --ptp
```

The IGC PMD requires a driver-specific timesync initialization order:

1. Configure the RX and TX queues.
2. Enable timesync before `rte_eth_dev_start()` so the PMD reserves the RX timestamp prefix.
3. Start the port.
4. Enable timesync again because IGC port start resets the timestamp registers.

Enabling timesync before queue setup can dereference unconfigured IGC RX queues. Enabling it
only after port start does not reserve the timestamp prefix and corrupts received packets. MTL
applies this sequence only to `MT_DRV_IGC`; the existing ICE/E800 initialization path is unchanged.

This path was validated with UDP/IPv4 multicast PTP in domain 0. Sync and Delay Request event
messages used UDP port 319, while Announce, Follow Up, and Delay Response general messages used
UDP port 320.

## 3. AF_XDP fallback flow

If DPDK PMD path is not suitable for your deployment, use AF_XDP backend.

### 3.1 Keep kernel `igc` driver attached

For AF_XDP, do **not** bind to `vfio-pci`; interface remains as Linux netdev.

```bash
ip -br link
ethtool -i <ifname>
```

### 3.2 Assign and verify interface IP

```bash
sudo ip addr add 192.168.88.101/24 dev <ifname>
sudo ip link set <ifname> up
ip -4 addr show dev <ifname>
```

### 3.3 Run with AF_XDP config

Use JSON profiles under RxTxApp AF_XDP examples and update interface names/IPs for your setup.
See also [AF_XDP guide](experimental/af_xdp.md).

For native AF_XDP, use the `native_af_xdp:` prefix. The I226-V exposes four combined queues in a
typical configuration. Queue 0 remains available to the kernel, leaving queues 1 through 3 for
AF_XDP. The following settings were validated for one audio TX session:

```json
{
    "shared_tx_queues": true,
    "interfaces": [
        {
            "name": "native_af_xdp:<ifname>",
            "ip": "192.168.88.101",
            "tx_queues_cnt": "2",
            "rx_queues_cnt": "1"
        }
    ]
}
```

### 3.4 Install and start MTL Manager

Install `mtl.xdp.o` in the same libxdp BPF object directory that contains
`xdp-dispatcher.o`. On Ubuntu multiarch installations this is commonly:

```bash
sudo install -m 0644 manager/build/mtl.xdp.o \
  /usr/lib/$(gcc -dumpmachine)/bpf/mtl.xdp.o
sudo MtlManager
```

Do not set `LIBXDP_OBJECT_PATH` to a directory that contains only `mtl.xdp.o`; doing so can hide
the system `xdp-dispatcher.o` and cause both native and SKB-mode attachment to fail.

The application needs `CAP_NET_RAW` for native AF_XDP:

```bash
sudo setcap 'cap_net_raw+ep' ./tests/tools/RxTxApp/build/RxTxApp
```

### 3.5 Use linuxptp with native AF_XDP

The kernel `igc` driver owns the PHC in native AF_XDP mode, so use one host-level `ptp4l` and
`phc2sys` pair instead of enabling MTL built-in PTP in each application. Multiple MTL applications
can then share the disciplined system clock.

Stop other services that discipline `CLOCK_REALTIME`; otherwise they will fight `phc2sys`:

```bash
sudo systemctl stop systemd-timesyncd
```

Start `ptp4l` as a hardware-timestamped, multicast UDP/IPv4, E2E client. Linuxptp 4.0 defaults to
PTP minor version 1. If the grandmaster or switch implements PTP v2.0 only, force minor version 0:

```bash
sudo ptp4l -i <ifname> -4 -E -H -s -m -q --ptp_minor_version 0
```

Without this override, a Delay Request has `minorVersionPTP=1` (the version byte is `0x12`). Some
PTP v2.0 devices ignore it, leaving `ptp4l` in `UNCALIBRATED` with repeated delay timeouts. With
minor version 0, the version byte is `0x02`, Delay Request/Response completes, and the port enters
`SLAVE`.

After `ptp4l` reaches `SLAVE`, synchronize the system clock. Choose the offset based on the
grandmaster timescale:

* For a standards-based PTP/TAI grandmaster that advertises a valid `currentUtcOffset`, let
  `phc2sys` obtain the offset from `ptp4l`:

  ```bash
  sudo phc2sys -s /dev/ptp0 -c CLOCK_REALTIME -w -m
  ```

* For a grandmaster whose numeric clock is UTC and which advertises an arbitrary/UTC timescale,
  keep PHC and `CLOCK_REALTIME` at the same epoch:

  ```bash
  sudo phc2sys -s /dev/ptp0 -c CLOCK_REALTIME -O 0 -w -m
  ```

  RxTxApp reads `CLOCK_TAI` by default, so pass the current TAI-to-UTC offset when media timestamps
  must match that UTC clock. The offset is 37 seconds as of June 2026:

  ```bash
  ./tests/tools/RxTxApp/build/RxTxApp \
    --config_file <native-af-xdp-config.json> \
    --utc_offset 37
  ```

Do not use `--ptp` in this AF_XDP configuration. PTP UDP ports 319 and 320 remain in the kernel;
the MTL XDP filter redirects only registered media destination ports to AF_XDP sockets.

### 3.6 Validated AF_XDP result

An I226-V at 1GbE was tested for 120 seconds with one 48 kHz, two-channel ST 2110-30 audio TX
session. The application exited successfully and held 1000 frames/s at 1.968 Mb/s. While the XDP
program was attached and media was transmitting, `ptp4l` remained in `SLAVE` with approximately
70 ns offset from the grandmaster and 109 ns mean path delay.

The tested teardown reported outstanding RX mbufs and full TX-ring warnings after media stopped.
They did not cause packet loss or a nonzero application exit during this run, but should be
considered separately when validating repeated start/stop cycles.

## 4. Validation matrix and day-1 command sequence

### 4.1 Validation matrix (1G and 2.5G)

Validate each mode at both link rates where applicable:

* DPDK backend @ 1G
* DPDK backend @ 2.5G
* AF_XDP backend @ 1G
* AF_XDP backend @ 2.5G

For each run collect:

* Packet loss
* End-to-end latency
* Jitter
* CPU utilization
* Soak stability (for example 8h/24h)

### 4.2 Day-1 command sequence

```bash
# 1) Inventory
lspci -nn | grep -i -E "ethernet|intel"
uname -a

# 2) NIC details
ethtool -i <ifname>
ethtool -k <ifname>
ethtool -T <ifname>

# 3) DPDK bind status
sudo dpdk-devbind.py -s

# 4) Link validation (1G/2.5G as required)
ethtool <ifname> | grep -E "Speed|Duplex|Link detected"

# 5) AF_XDP PTP sanity (PTP v2.0 grandmaster)
sudo ptp4l -i <ifname> -4 -E -H -s -m --ptp_minor_version 0
sudo phc2sys -s <ifname> -c CLOCK_REALTIME -m -w
```
