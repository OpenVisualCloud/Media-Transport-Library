# Media Transport Library compilation and build Guide on Windows OS (MSYS2)

**Note:** Support for Windows has been discontinued. If you want to run MTL on Windows, it may be possible, but no guaranties.

## 1. Introduction

This document contains instructions for installing and configuring the Media Transport Library for Windows Operation System in MSYS2 environment.

## 2. Prerequisites

* Windows 10 / Windows Server 2019 64-bit or higher

## 3. Install MSYS2 environment

* Download and install MSYS2 from <https://www.msys2.org/>.
* Open an MSYS2 MINGW64/UCRT64 shell, all commands in this doc will be run in this shell.
* Update packages:

```bash
pacman -Syuu
```

## 4. Install dependencies

* Install build tools and dependencies:

```bash
pacman -S git base-devel unzip pactoys
pacboy -S gcc:p meson:p pkgconf:p openssl:p json-c:p libpcap:p dlfcn:p SDL2:p SDL2_ttf:p gtest:p
```

## 4. Install tools

* Download and install npcap from <https://npcap.com/dist/npcap-1.60.exe>.

* Install npcap SDK:

```bash
wget https://nmap.org/npcap/dist/npcap-sdk-1.12.zip
unzip -d npcap-sdk npcap-sdk-1.12.zip
cp npcap-sdk/Lib/x64/* $MSYSTEM_PREFIX/lib/
```

* Install mman (mmap for Windows):

```bash
git clone https://github.com/alitrack/mman-win32
cd mman-win32
./configure --prefix=$MSYSTEM_PREFIX
make && make install
```

## 5. Build DPDK

**Note:** DPDK 23.11 was the last version to which DPDK patches were ported.

* Clone the MTL repository if not:

```bash
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
export mtl_source_code=${PWD}/Media-Transport-Library
```

* Convert symlink patch files to real file:

```bash
cd $mtl_source_code/patches/dpdk/23.11
ls *.patch | xargs -I{} bash -c 'if [[ $(sed -n '1p' "{}") =~ ^../.*\.patch$ ]]; then cp "$(cat "{}")" "{}"; fi'
cd windows
ls *.patch | xargs -I{} bash -c 'if [[ $(sed -n '1p' "{}") =~ ^../.*\.patch$ ]]; then cp "$(cat "{}")" "{}"; fi'
```

* Clone the DPDK repository and apply patches:

```bash
cd $mtl_source_code
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v23.11
git switch -c v23.11

git config user.name "Your Name"        # config if not
git config user.email "you@example.com" # config if not
git am $mtl_source_code/patches/dpdk/23.11/*.patch
git am $mtl_source_code/patches/dpdk/23.11/windows/*.patch
```

* Build and install DPDK:

```bash
meson setup build
meson install -C build
```

## 6. Build Media Transport Library and app

```bash
cd $mtl_source_code
./build.sh
```

## 7. Add MSYS2 binary PATH to system environment variables (Optional)

The MSYS2 path is not in Windows system environment variables by default, if you want to run MTL apps in PowerShell/CMD, you need to add the paths first. For example, MSYS2 is installed in `C:\msys64`.

* (optional)Add MSYS2 common toolchain path: `C:\msys64\usr\bin`

* If the environment is MinGW64, add: `C:\msys64\mingw64\bin`

* If the environment is UCRT64, add: `C:\msys64\ucrt64\bin`
