@page build_linux Build in Linux
# Build Guide

The build required 3 parts, build the DPDK library, the Kahawai library on top of DPDK, and the sample application.

## 1. Install the build dependency
#### 1.1 Ubuntu/Debian:
```bash
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev
sudo pip3 install pyelftools
```
#### 1.2 Centos:
```bash
sudo yum install git gcc meson python3 python3-pip pkg-config libnuma-devel json-c-devel libpcap-devel gtest-devel SDL2-devel SDL2_ttf-devel openssl-devel
sudo pip3 install pyelftools
```

## 2. DPDK build and install:

#### 2.1 Get DPDK 21.11 source
```bash
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v21.11
git switch -c v21.11
```

#### 2.2 Apply the DPDK patches required to run kahawai.
Note: $dpdk_st_kahawai point to source code of ST DPDK kahawai.
```bash
git am $dpdk_st_kahawai/patches/dpdk/21.11/0001-build-enable-IEEE1588-PTP-option.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0002-Change-to-enable-PTP.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0003-net-ice-support-max-burst-size-configuration.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0004-eanble-PF-ICE-rate-limit.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0005-update-VF-rate-limit.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0006-net-iavf-support-max-burst-size-configuration.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0007-pcapng-add-ns-timestamp-for-copy-api.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0008-net-ice-support-256-queues.patch
git am $dpdk_st_kahawai/patches/dpdk/21.11/0009-net-iavf-refine-queue-rate-limit-configure.patch
```

#### 2.3 Build and install DPDK library
```bash
meson build
ninja -C build
cd build
sudo ninja install
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
pkg-config --modversion libdpdk
```

## 3. DPDK ST kahawai lib and app:
```bash
./build.sh
```
