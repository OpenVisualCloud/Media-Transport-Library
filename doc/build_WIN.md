# Intel® Media Transport Library compilation and build on Windows OS (MSYS2)

## 1. Introduction

This document contains instructions for installing and configuring the Intel® Media Transport Library for Windows Operation System in MSYS2 environment.

## 2. Prerequisites

* Windows 10 / Windows Server 2019 64-bit or higher

## 3. Install MSYS2 environment

* Download and install MSYS2 from <https://www.msys2.org/>.
* Open an MSYS2 MINGW64/UCRT64 shell, all commands in this doc will be run in this shell.
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

* Clone the IMTL repository if not:

```bash
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
cd Media-Transport-Library
```

For all steps below, the default work dir is IMTL repository.

* Convert symlink patch files to real file:

```bash
cd patches/dpdk/23.03
ls *.patch | xargs -I{} bash -c 'if [[ $(sed -n '1p' "{}") =~ ^../.*\.patch$ ]]; then cp "$(cat "{}")" "{}"; fi'
cd windows
ls *.patch | xargs -I{} bash -c 'if [[ $(sed -n '1p' "{}") =~ ^../.*\.patch$ ]]; then cp "$(cat "{}")" "{}"; fi'
```

* Clone the DPDK repository and apply patches:

```bash
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v23.03
git switch -c v23.03

git config user.name "Your Name"        # config if not
git config user.email "you@example.com" # config if not
git am ../patches/dpdk/23.03/*.patch
git am ../patches/dpdk/23.03/windows/*.patch
```

* Build and install DPDK:

```bash
cd dpdk
meson setup build
meson install -C build
```

## 6. Build Intel® Media Transport Library and app

```bash
./build.sh
```

## 7. Add MSYS2 binary PATH to system environment variables (Optional)

The MSYS2 path is not in Windows system environment variables by default, if you want to run IMTL apps in PowerShell/CMD, you need to add the paths first. For example, MSYS2 is installed in `C:\msys64`.

* (optional)Add MSYS2 common toolchain path: `C:\msys64\usr\bin`

* If the environment is MinGW64, add: `C:\msys64\mingw64\bin`

* If the environment is UCRT64, add: `C:\msys64\ucrt64\bin`
