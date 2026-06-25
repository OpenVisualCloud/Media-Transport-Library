# Build Guide for FreeBSD

This guide provides instructions for building the Media Transport Library (MTL) on FreeBSD.

## Requirements

- FreeBSD 14.x or later (recommended for best DPDK support)
- x86_64 architecture (required for SIMD optimizations)
- Intel E810 or E830 NIC (recommended for hardware rate limiting / narrow pacing)

## 1. Prerequisites

### 1.1. Install Build Dependencies

```bash
# Essential build tools
pkg install -y git gcc meson pkgconf ninja python3 py311-pyelftools

# Core libraries
pkg install -y json-c libpcap

# NUMA support (optional but recommended for multi-socket systems)
pkg install -y numa

# Testing frameworks (optional)
pkg install -y googletest

# SDL2 for RxTxApp display support (optional)
pkg install -y sdl2 sdl2_ttf

# LLVM/Clang for optimizations
pkg install -y llvm clang
```

> Note: `py311-pyelftools` is required by DPDK v25.11's Meson build system. The package name is tied to the Python 3.11 slot; if your system uses a different Python slot, adjust accordingly (e.g. `py312-pyelftools`).
>
> To automate prerequisite installation (including `pyelftools` slot handling), run:
> `sudo ./scripts/freebsd_required.sh`

### 1.2. Verify DTrace Support

FreeBSD has native DTrace support for USDT tracepoints:

```bash
# Check if dtrace is available
which dtrace

# Expected: /usr/sbin/dtrace
```

### 1.3. Clone Media Transport Library Code

```bash
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
cd Media-Transport-Library
export mtl_source_code=$(pwd)
```

## 2. Configure Hugepages

MTL requires hugepages (2MB recommended) for optimal performance.

### 2.1. Configure Hugepages at Boot

Add to `/boot/loader.conf`:

```bash
# Enable superpages
vm.pmap.pg_ps_enabled=1

# Configure contigmem for DPDK (1024 × 2MB = 2GB)
hw.contigmem.num_buffers=1024
hw.contigmem.buffer_size=2097152
```

### 2.2. Load contigmem Module

```bash
# Load immediately
kldload contigmem

# Make persistent across reboots
echo 'contigmem_load="YES"' >> /boot/loader.conf

# Reboot to apply all settings
reboot
```

### 2.3. Verify Hugepages Configuration

```bash
# Check contigmem status
sysctl -a | grep contigmem

# Expected output:
# hw.contigmem.num_buffers: 1024
# hw.contigmem.buffer_size: 2097152
```

## 3. Build DPDK

### 3.1. Get DPDK 25.11 Source Code

```bash
cd $mtl_source_code
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v25.11
git switch -c v25.11
cd ..
```

### 3.2. Apply MTL DPDK Patches

Apply the platform-agnostic patches from MTL:

```bash
cd dpdk

# Apply hardware-related patches (E810/E830 tuning)
git am $mtl_source_code/patches/dpdk/25.11/0001-e810-set-max-ring-desc-to-max-allowed-by-hardware.patch
git am $mtl_source_code/patches/dpdk/25.11/0002-net-iavf-refine-queue-rate-limit-configure.patch
git am $mtl_source_code/patches/dpdk/25.11/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch
git am $mtl_source_code/patches/dpdk/25.11/0004-Change-to-enable-PTP.patch
git am $mtl_source_code/patches/dpdk/25.11/0005-iavf-disable-runtime-queue.patch
git am $mtl_source_code/patches/dpdk/25.11/0006-pcapng-add-user-timestamp-support.patch
git am $mtl_source_code/patches/dpdk/25.11/0007-e830-Fix-ice_ptp_adj_clock.patch

# Note: Skip Linux-specific patches (AF_XDP, header-split)
# Note: Skip Windows-specific patches
```

### 3.3. Build and Install DPDK

```bash
# Configure DPDK for FreeBSD
meson setup build \
  -Dmax_lcores=256 \
  -Dplatform=generic

# Build
ninja -C build

# Install (requires root)
sudo ninja install -C build
cd ..
```

**Notes:**
- Verify `numa` library detection in meson output (optional but recommended)

### 3.4. Verify DPDK Installation

```bash
# Check installed libraries
pkg-config --libs libdpdk

# Test DPDK EAL initialization
cd dpdk/build/examples/helloworld
sudo ./dpdk-helloworld -l 0-1 -n 4

# Expected output: "hello from core 0" and "hello from core 1"
```

## 4. Build Media Transport Library

### 4.1. Set Environment

```bash
cd $mtl_source_code

# Ensure pkg-config can find DPDK
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
```

### 4.2. Build MTL

```bash
# Use the build script (handles meson configuration)
./build.sh

# Or manually with meson:
meson setup build \
  -Denable_asan=false \
  -Denable_usdt=true \
  -Denable_fuzzing=false

ninja -C build
sudo ninja install -C build
```

### 4.3. Verify MTL Build

```bash
# Check shared library
ls build/lib/libmtl.so*

# Check symbols
nm build/lib/libmtl.so 2>/dev/null | grep mtl_init

# Expected: Symbol for mtl_init should be present
```

## 5. Build Sample Applications

### 5.1. Build RxTxApp

```bash
# RxTxApp is built automatically with MTL
ls build/tests/tools/RxTxApp/RxTxApp

# Test help output
./build/tests/tools/RxTxApp/RxTxApp --help
```

## 6. NIC Driver Configuration

### 6.1. Option A: Use DPDK ice PMD (Recommended)

Unbind NIC from FreeBSD kernel driver:

```bash
# Find your NIC's PCI address
pciconf -lv | grep -A3 "Ethernet"

# Example: pci0:175:0:0 for device ice0

# Unbind from kernel driver
sudo devctl detach pci0:175:0:0

# DPDK will now use the PMD directly via nic_uio or contigmem
```

### 6.2. Option B: Use FreeBSD ice(4) Kernel Driver

Keep NIC attached to kernel for testing with kernel socket backend:

```bash
# No unbinding needed
# MTL can use "kernel:ice0" backend (slower but simpler)
```

## 7. FAQ

### 7.1. PKG_CONFIG_PATH Issue

If you get dependency errors:

```bash
# Find libdpdk.pc location
find /usr/local -name libdpdk.pc

# Set PKG_CONFIG_PATH
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig
```

### 7.2. NUMA Library Not Found

NUMA support is optional on FreeBSD. If `libnuma` is not available:

```bash
# MTL will use single-socket fallback automatically
# This is acceptable for single-socket systems

# For multi-socket systems, install numa:
pkg install numa
```

### 7.3. Build with Clang

FreeBSD defaults to Clang. To explicitly use GCC:

```bash
export CC=gcc CXX=g++
rm build -rf
./build.sh
```

### 7.4. Contigmem Permission Issues

If DPDK complains about hugepages:

```bash
# Check contigmem device permissions
ls -l /dev/contigmem*

# Ensure user is in wheel or operator group
sudo pw groupmod wheel -m $USER

# Or run with sudo for testing
sudo ./RxTxApp ...
```

## 8. FreeBSD-Specific Limitations

### 8.1. AF_XDP Backend Not Available

AF_XDP is Linux-specific and disabled on FreeBSD. Use DPDK PMD backend instead:

- ✅ **DPDK PMD**: Full performance, hardware rate limiting
- ✅ **Kernel Socket**: Testing/development, no hardware RL
- ❌ **AF_XDP**: Not available on FreeBSD

### 8.2. eBPF Tools Not Available

The monitoring tools in `tools/ebpf/` are Linux-specific. Use FreeBSD alternatives:

- DTrace for tracing (native FreeBSD support)
- `pmcstat` for performance monitoring
- `netstat`, `systat` for network stats

## Next Steps

Proceed to [Running MTL on FreeBSD](./run_FREEBSD.md) for deployment instructions.
