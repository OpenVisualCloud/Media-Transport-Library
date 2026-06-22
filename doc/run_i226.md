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
* Verify kernel command line has IOMMU enabled:

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

# 5) PTP sanity (if using ptp4l/phc2sys)
sudo ptp4l -i <ifname> -m
sudo phc2sys -s <ifname> -c CLOCK_REALTIME -m
```
