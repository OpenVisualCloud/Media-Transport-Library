@page build_linux Build in Linux

# Build Guide

The build required 3 parts, build the DPDK library, the Kahawai library on top of DPDK, and the sample application.

## 1. Install the build dependency

#### 1.1 Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev
sudo pip install pyelftools ninja
```

#### 1.2 Centos

```bash
sudo dnf config-manager --set-enabled powertools
sudo yum install git gcc gcc-c++ meson python3 python3-pip pkg-config json-c-devel libpcap-devel gtest-devel SDL2-devel openssl-devel numactl-devel libasan
sudo pip3 install pyelftools
```

## 2. DPDK build and install

#### 2.1 Get DPDK 22.11 source code

```bash
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v22.11
git switch -c v22.11
```

#### 2.2 Apply the DPDK patches required to run kahawai

Note: $dpdk_st_kahawai point to source code of ST DPDK kahawai.

```bash
git am $dpdk_st_kahawai/patches/dpdk/22.11/0001-pcapng-add-ns-timestamp-for-copy-api.patch
git am $dpdk_st_kahawai/patches/dpdk/22.11/0002-net-af_xdp-parse-numa-node-id-from-sysfs.patch
git am $dpdk_st_kahawai/patches/dpdk/22.11/0003-net-iavf-refine-queue-rate-limit-configure.patch
git am $dpdk_st_kahawai/patches/dpdk/22.11/0004-net-ice-revert-PF-ICE-rate-limit-to-non-queue-group-.patch
git am $dpdk_st_kahawai/patches/dpdk/22.11/0005-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch
git am $dpdk_st_kahawai/patches/dpdk/22.11/0006-Change-to-enable-PTP.patch
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

## 3. DPDK ST kahawai lib and app

```bash
./build.sh
```

## 4. FAQ

#### 4.1 PKG_CONFIG_PATH issue

It may get below error caused by PKG_CONFIG_PATH path problem.

```bash
lib/meson.build:10:0: ERROR: Dependency "libdpdk" not found, tried pkgconfig
```

Try below command to find the pc path and add to the PKG_CONFIG_PATH env.

```bash
find / -name libdpdk.pc
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/:/usr/local/lib/pkgconfig/
```

#### 4.2 Build with clang

Before build, export CC/CXX to clang, see below for example.

```bash
export CC=clang CXX=clang++
rm build -rf
./build.sh
```
