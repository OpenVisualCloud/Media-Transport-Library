# Intel® Media Transport Library compilation and build on Windows OS (MSYS2)

## 1. Introduction

This document contains instructions for installing and configuring the Intel® Media Transport Library for Windows Operation System in MSYS2 environment.

## 2. Prerequisites

* Windows 10 / Windows Server 2019 64-bit or higher

## 3. Install MSYS2 environment

* Download and install MSYS2 from <https://www.msys2.org/>.
* Open an MSYS2 MINGW64/UCRT64 shell, all commands in this doc will be run in this shell, the default work dir is IMTL repository.
* Update packages:

```bash
pacman -Syu
```

## 4. Install dependencies

* Install build tools and dependencies:

```bash
pacman -S git base-devel unzip pactoys
pacboy -S openssl:p gcc:p meson:p pkg-config:p json-c:p libpcap:p gtest:p SDL2:p SDL2_ttf:p dlfcn:p
```

## 4. Install tools

* Install mman (mmap for windows):

```bash
git clone https://github.com/alitrack/mman-win32
cd mman-win32
./configure --prefix=$MSYSTEM_PREFIX
make && make install
```

* Install npcap SDK:

```bash
wget https://nmap.org/npcap/dist/npcap-sdk-1.12.zip
unzip -d npcap-sdk npcap-sdk-1.12.zip
cp npcap-sdk/Lib/x64/* $MSYSTEM_PREFIX/lib/
```

* Download and install npcap from <https://npcap.com/dist/npcap-1.60.exe>.

## 5. Build DPDK

* Clone the DPDK repository:

```bash
git clone https://github.com/DPDK/dpdk.git
```

* Build DPDK:

```bash
cd dpdk
meson setup build
meson install -C build
```

## 6. Build IMTL

* Build and install IMTL lib:

```bash
meson setup build
meson install -C build
```

* Build IMTL app:

```bash
cd app
meson setup build
meson compile -C build
```

* Build IMTL tests:

```bash
cd tests
meson setup build
meson compile -C build
```

* Build and install IMTL plugins:

```bash
cd plugins
meson setup build
meson install -C build
```
