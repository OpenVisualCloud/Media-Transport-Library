@page build_linux Build in Linux
# Build Guide

The build required 3 parts, build the DPDK library, the ST Kahawai library on top of DPDK, and the sample application.

## 1. Install the build dependency
#### 1.1 Ubuntu/Debian:
```bash
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev
sudo pip3 install pyelftools
```
#### 1.2 Centos:
```bash
sudo yum install git gcc meson python3 python3-pip pkg-config libnuma-devel json-c-devel libpcap-devel gtest-devel SDL2-devel
sudo pip3 install pyelftools
```

## 2. DPDK build and install:

#### 2.1 Get DPDK 21.08 source
```bash
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v21.08
git switch -c v21.08
```

#### 2.2 Apply the DPDK patches required to run kahawai.
Note: $dpdk_st_kahawai point to source code of ST DPDK kahawai.
```bash
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0001-update-ice-1588-timesync-based-on-21.08.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0002-temp-fix-to-enable-multicast-rx-on-ice.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0003-fix-L4-packet-not-work.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0004-eanble-PF-ICE-rate-limit.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0005-update-VF-rate-limit.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0006-net-iavf-add-capability-for-runtime-rx-tx-queue-setu.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0007-build-enable-IEEE1588-PTP-option.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0008-enable-rx-timestamp-and-fix-performance-issue.patch
patch -p1 < $dpdk_st_kahawai/patches/dpdk/21.08/0009-net-ice-support-max-burst-size-configuration.patch
```

#### 2.3 Build and install DPDK library
```bash
meson build
ninja -C build
cd build
sudo ninja install
sudo ldconfig
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
```

## 3. DPDK ST kahawai lib and app:
```bash
./build.sh
```
