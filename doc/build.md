# Build Guide

Building the Media Transport Library requires three parts: building the DPDK library, building the Media Transport Library on top of DPDK, and building the sample application.

Please note that the DPDK dependency remains necessary when utilizing the XDP/kernel socket data path backend. We leverage numerous DPDK APIs, including those for CPU, memory, queues, and mbufs, to construct a highly efficient network processing implementation.

## 1. Prerequisites

### 1.1. Install the build dependency from OS software store

#### 1.1.1. Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev systemtap-sdt-dev llvm clang flex byacc
sudo pip install pyelftools ninja
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo apt-get install libsdl2-dev libsdl2-ttf-dev
```

#### 1.1.2. CentOS stream

```bash
sudo yum install -y dnf-plugins-core
sudo dnf config-manager --set-enabled powertools
sudo yum install git gcc gcc-c++ meson python3 python3-pip pkg-config json-c-devel libpcap-devel gtest-devel openssl-devel numactl-devel libasan systemtap-sdt-devel clang llvm-devel
sudo pip3 install pyelftools
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo yum install SDL2-devel
```

#### 1.1.3. RHEL 9

```bash
sudo yum install git gcc gcc-c++ python3 python3-pip pkg-config SDL2-devel openssl-devel numactl-devel libasan systemtap-sdt-devel
sudo pip3 install meson ninja pyelftools
```

RHEL 9 doesn't provide `json-c-devel libpcap-devel gtest-devel` package as default, it has to build these three libs from source code, install below dependency and follow section [1.2. Build dependency from source code](#12-build-dependency-from-source-code) to build.

```bash
sudo yum install cmake flex bison
```

#### 1.1.4. Arch Linux

```bash
sudo pacman -Syu --needed git gcc meson python python-pyelftools pkg-config json-c libpcap gtest openssl numactl
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo pacman -Syu --needed  sdl2 sdl2_ttf
```

### 1.2. Build dependency from source code

It's true that not all operating systems, including RHEL 9, come with all the libraries required for every software package. If you're trying to install a software that has dependencies not provided by default by your package manager, you might need to install these dependencies manually. Skip these step if your setup has all dependencies resolved.

Refer to below commands for how to build from code.

#### 1.2.1. json-c

```bash
git clone https://github.com/json-c/json-c.git -b json-c-0.16
cd json-c/
mkdir build
cd build
cmake ../
make
sudo make install
cd ../../
```

#### 1.2.2. libpcap

```bash
git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-1.9
cd libpcap/
./configure
make
sudo make install
cd ..
```

#### 1.2.3. gtest

```bash
git clone https://github.com/google/googletest.git -b v1.13.x
cd googletest/
mkdir build
cd build/
cmake ../
make
sudo make install
cd ../../
```

### 1.3. secure_path for root user

The build steps use `sudo ninja install` to install the build to the system. Some operating systems, including CentOS Stream and RHEL 9, do not have `/usr/local/bin/` in the secure_path defaults.

Edit the file `/etc/sudoers`, find `secure_path` and append `/usr/local/bin`

```bash
Defaults    secure_path = /sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
```

Reboot the system to let change take effect.

### 1.4. Clone Media Transport Library code

Download Media Transport Library to top folder Directory

```bash
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
export mtl_source_code=${PWD}/Media-Transport-Library
```

## 2. DPDK build and install

### 2.1. Get DPDK 25.11 source code

```bash
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v25.11
git switch -c v25.11
cd ..
```

### 2.2. Apply the DPDK patches required to run Media Transport Library

Note: $mtl_source_code should be pointed to top source code tree of Media Transport Library.

```bash
cd dpdk
git am $mtl_source_code/patches/dpdk/25.11/*.patch
```

### 2.3. Build and install DPDK library

```bash
meson setup build
ninja -C build
sudo ninja install -C build
cd ..
```

Note, please make sure libnuma is installed, confirm it from the log of `meson setup build` command.

```text
Library numa found: YES
```

If you see below log from `sudo ninja install -C build`, it seems you're encountering an issue where the ninja command isn't recognized. This problem is likely due to the ninja executable not being in your system's PATH, refer to `### 1.3`

```bash
sudo: ninja: command not found
```

## 3. Build Media Transport Library and app

```bash
cd $mtl_source_code
./build.sh
```

## 4. FAQ

### 4.1. PKG_CONFIG_PATH issue

It may get below error caused by PKG_CONFIG_PATH path problem.

```text
lib/meson.build:10:0: ERROR: Dependency "libdpdk" not found, tried pkgconfig
```

Try below command to find the pc path and add to the PKG_CONFIG_PATH env.

```bash
find / -name libdpdk.pc
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/:/usr/local/lib/pkgconfig/
```

### 4.2. Build with clang

Before build, export CC/CXX to clang, see below for example.

```bash
export CC=clang CXX=clang++
rm build -rf
./build.sh
```

### 4.3. Build portable package

Sometimes, you may need to create a portable package that can be used directly on other nodes. Please note that the default steps in section `### 2.3` utilize the `-march=native` compiler version. This generates native instructions based on local CPU features, some of which may not exist on other devices. In such scenarios, it's necessary to specifically indicate the CPU instructions you want to support.

Below are the steps to generate a DPDK binary capable of AVX2. Set `cpu_instruction_set` to `haswell` during the DPDK meson setup stage.

```bash
cd dpdk
rm build -rf
meson setup build -Dcpu_instruction_set=haswell
ninja -C build
sudo ninja install -C build
cd ..
```

Next, rebuild the Media Transport Library. MTL will reuse the build flags from DPDK.

```bash
cd $mtl_source_code
rm build -rf
./build.sh
```

If you want to enable a AVX512 capable build, just set `cpu_instruction_set` to `skylake-avx512`:

```bash
meson setup build -Dcpu_instruction_set=skylake-avx512
```

Use the command below to check the cflags (march) used for the DPDK build:

```bash
pkg-config --cflags libdpdk
# below output indicate it use haswell arch
-include rte_config.h -march=haswell -msse4 -I/usr/local/include
```

Use below command to check the detail compiler flags of one `march`:

```bash
echo | gcc -dM -E - -march=haswell
```

### 4.4. Older kernel installed on the OS

In case older kernel installed on the OS throws the “Kernel header files not in any of expected locations”
and causes build errors, you can try to fix it with installation of kernel specific packet `kernel-devel`:

```bash
# e.g. for CentOS or RHEL
sudo yum install "kernel-devel-$(uname -r)"
```

or with:
```bash
# e.g. for Ubuntu
sudo apt-get install linux-headers-$(uname -r)
```

For older kernel version on Red Hat, the issue is that Red Hat uses vault repos. Older versions are stored in the vaults and the correct repository needs to be enabled to install the targeted version.

[The exemplary location for Rocky Linux](https://dl.rockylinux.org/vault/rocky/)

[The exemplary location for the kernel-devel package for Rocky Linux 9.3](https://dl.rockylinux.org/vault/rocky/9.3/BaseOS/x86_64/os/Packages/k/)

## Next Steps
Proceed to [Running MTL](./run.md) for further instructions.
