# Build Guide

Building the Intel® Media Transport Library requires three parts: building the DPDK library, building the Intel® Media Transport Library on top of DPDK, and building the sample application.

## 1. Install the build dependency

### 1.1 Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev
sudo pip install pyelftools ninja
```

### 1.2 Centos

```bash
sudo yum install git gcc gcc-c++ meson python3 python3-pip pkg-config json-c-devel libpcap-devel gtest-devel SDL2-devel openssl-devel numactl-devel libasan
sudo pip3 install pyelftools
```

## 2. DPDK build and install

### 2.1 Get DPDK 23.03 source code

```bash
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout -b v23.03
```

#### 2.1.1 Download Media Transport Library to top folder Directory

```bash
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
export imtl_source_code=${PWD}/Media-Transport-Library
```

### 2.2 Apply the DPDK patches required to run Intel® Media Transport Library

Note: $imtl_source_code point to source code of Intel® Media Transport Library.

```bash
cd dpdk
git am $imtl_source_code/patches/dpdk/23.03/*.patch
```

### 2.3 Build and install DPDK library

```bash
meson build
ninja -C build
cd build
sudo ninja install
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
pkg-config --modversion libdpdk
```

## 3. Build Intel® Media Transport Library and app

```bash
cd $imtl_source_code
./build.sh
```

## 4. FAQ

### 4.1 PKG_CONFIG_PATH issue

It may get below error caused by PKG_CONFIG_PATH path problem.

```bash
lib/meson.build:10:0: ERROR: Dependency "libdpdk" not found, tried pkgconfig
```

Try below command to find the pc path and add to the PKG_CONFIG_PATH env.

```bash
find / -name libdpdk.pc
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/:/usr/local/lib/pkgconfig/
```

### 4.2 Build with clang

Before build, export CC/CXX to clang, see below for example.

```bash
export CC=clang CXX=clang++
rm build -rf
./build.sh
```
