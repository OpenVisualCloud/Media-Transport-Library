# Intel(R) Media Streaming Library for ST 2110 compilation and build on Windows OS

## 1. Introduction

This document contains instructions for installing and configuring the Intel(R) Media Streaming Library for ST 2110 
for Windows Operation System.
This document describes how to compile and run a Intel(R) Media Streaming Library for ST 2110 and sample application 
in a Windows OS environment. 

All the steps below related to the DPDK configuration instructions on Windows are from the website:
https://doc.dpdk.org/guides/windows_gsg/intro.html

### 1.1 OS

- Tested on: Windows Server 2022

## 2. Preparation of the environment 

To prepare the environment, follow the instructions on the DPDk windows page.
The recommended options is:
"MinGW-w64 Toolchain"

### 2.1 PATH environmental variable in Windows
After downloading and installing Meson and Ninja you must add them to the PATH environmental variable in Windows. 
Search for edit the system environment variables and run the program. 
Click on Environment Variables... button and then in the upper section click on Path variable and click edit. 
Then add it to your environment variable by clicking new and writing the path down to them just like in the example below.

meson version 0.57.0 is recommanded version

for example:
```
C:\Program Files\Meson
C:\Ninja
```

## 3. Installation and configuration of DPDK
First install compile tools mingw64, suggest to install to c:\mingw64
https://sourceforge.net/projects/mingw-w64/files/Toolchains%20targetting%20Win64/Personal%20Builds/mingw-builds/8.1.0/threads-posix/sjlj/x86_64-8.1.0-release-posix-sjlj-rt_v6-rev0.7z

### 3.1 Preparation of sources

##### 3.1.1 Download DPDK sources from:

https://fast.dpdk.org/rel/dpdk-22.07.tar.xz

##### 3.1.2 Unpack the sources
To unpack you can use 7-zip:

•	Right-click the file

•	Select 7-zip -> Extract Here / Extract to

##### 3.1.3 Patch dpdk-22.07 with custom patches

Install git for Windows version
Run the following commands using gitbash:
Execute below command in Git bash
```
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0001-pcapng-add-ns-timestamp-for-copy-api.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0002-net-af_xdp-parse-numa-node-id-from-sysfs.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0003-net-iavf-refine-queue-rate-limit-configure.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0004-net-ice-revert-PF-ICE-rate-limit-to-non-queue-group-.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0005-net-iavf-support-max-burst-size-configuration.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0006-net-ice-support-max-burst-size-configuration.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0007-Add-support-for-i225-IT-ethernet-device-into-igc-pmd.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0008-Change-to-enable-PTP.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0009-ice-fix-ice_interrupt_handler-panic-when-stop.patch
```
Then apply windows platform dpdk patch
```
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0001-Add-DDP-package-load-support-in-windows.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0002-Change-list-remove-and-add-position-to-avoid-race-co.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0003-Mingw-compiler-do-have-same-implementation.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0004-Mingw-do-have-pthread-implementation-change-to-adapt.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0005-To-change-for-windows-trained-pad_interval-pass-in-v.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0006-Add-Windows-10-May-2019-and-newer-version-1GB-huge-p.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0007-Enable-descriptor-prefetch-for-CBDMA-version-3.4.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0008-Windows-need-set-the-timer-resolution-to-maximum-to-.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0009-Windows-dsa-driver-need-set-to-reset-status-first.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0010-Windows-version-currently-no-BPF-support.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0011-Remove-affinity-binding-for-windows-will-have-perfor.patch
```

### 3.2 Build and install DPDK
Execute the commands below in windows command prompt:
```
cd  C:\path_to\dpdk-21.11
meson build --prefix=c:\dpdk
ninja -C build install 
```
#### 3.2.1 Delete all *.dll.a files in c:\dpdk\lib, and only keep *.a files

#### 3.2.2 Find C:\dpdk\lib\pkgconfig\libdpdk.pc, change the libs define to below
Libs.private:

#### 3.2.3 Find C:\dpdk\lib\pkgconfig\libdpdk-libs.pc, change the libs define to below
```
Libs:-L${libdir} -lrte_latencystats -lrte_gso -lrte_bus_pci -lrte_gro -lrte_cfgfile -lrte_bitratestats -lrte_timer -lrte_hash -lrte_metrics -lrte_cmdline -lrte_pci -lrte_ethdev -lrte_meter -lrte_net -lrte_net_ice -lrte_net_iavf -lrte_common_iavf -lrte_mbuf -lrte_mempool -lrte_stack -lrte_mempool_stack -lrte_mempool_ring -lrte_rcu -lrte_ring -lrte_eal -lrte_telemetry -lrte_kvargs -lrte_dmadev -lrte_dma_ioat
```

## 3.3 Add pkg-config

##### 3.3.1 Download three folders:
pkg-config_0.26-1
https://sourceforge.net/projects/pkgconfiglite/files/0.26-1/pkg-config-lite-0.26-1_bin-win32.zip/download 

glib_2.28.8-1_win32
https://download.gnome.org/binaries/win32/glib/2.28/  

gettext-runtime_0.18.1.1-2_win32
https://www.npackd.org/p/org.gnu.GetTextRuntime/0.18.1.1.2

##### 3.3.2 Install them

##### 3.3.3 Add path to the environment variable as described below just like python, meson and ninja:

```
C:\path_to\pkg-config_0.26-1\bin\
C:\path_to\glib_2.28.8-1_win32\bin\
C:\path_to\gettext-rintime_0.18.1.1-2_win32\bin\
```

##### 3.3.4 Download SDL2 folder

Download from: 
https://www.libsdl.org/download-2.0.php

##### 3.3.5 Create new environment variable:
PKG_CONFIG_PATH is correct installed and compiled package path for pkg-config searching
For example, c:\sdl2\lib\pkgconfig

##### 3.3.6 Change the sdl2.pc file
Change libdir and includedir to your correct install path
Also, change Cflags: -I${includedir}  -Dmain=SDL_main

##### 3.3.7 Download json-c folder

Download from: 
https://packages.msys2.org/package/mingw-w64-x86_64-json-c
Uzip the zst file, remove *.dll.a file in lib folder, only keep *.a file, or else you will need json-c.dll when running

##### 3.3.8 Add the json-c install pkgconfig path to the PKG_CONFIG_PATH environment variable:
For example, c:\json-c\lib\pkgconfig

##### 3.3.9 Change the json-c.pc file
Change libdir and includedir to your correct install path

##### 3.3.10 Download win-pcap folder

Download from: 
https://nmap.org/npcap/dist/npcap-sdk-1.12.zip,
Uzip the zip file

Download from: 
https://npcap.com/dist/npcap-1.60.exe,
Install the package on the target running machine

##### 3.3.11 Copy the npcap x64 directory lib files to mingw lib directory,such as
C:\mingw64\lib

##### 3.3.12 Copy the npcap include header files to mingw include directory,such as
C:\mingw64\x86_64-w64-mingw32\include

##### 3.3.13 Download mman folder

Download from: 
https://github.com/alitrack/mman-win32
Uzip the zip file, go to the directory, Execute below command in Git bash
```
./configure
/c/mingw64/bin/mingw32-make.exe libmman.a
```
##### 3.3.14 Copy the compiled libmman.a files to mingw lib directory,such as
C:\mingw64\lib

##### 3.3.15 Copy the mman include header files to mingw include directory,such as
C:\mingw64\x86_64-w64-mingw32\include\sys

##### 3.3.16 Download gtest folder

Download from: 
https://packages.msys2.org/package/mingw-w64-x86_64-gtest
Uzip the zst file

##### 3.3.17 Add the gtest install pkgconfig path to the PKG_CONFIG_PATH environment variable:

##### 3.3.18 Change the gtest.pc file
Change libdir and includedir to your correct install path

Download the gtest Dependencies: mingw-w64-x86_64-gcc-libs from: 
https://packages.msys2.org/package/mingw-w64-x86_64-gcc-libs
Uzip the zst file and copy the libstdc++-6.dll to the directory same as gtest.dll, 
gtest can now only link with dll mode, when you run the app, you need copy gtest.dll and libstdc++-6.dll to the directory 
same as your app

Note: please keep the *.dll.a files of the gtest, gtest can not be static linked, do not use libstdc++-6.dll in mingw64 directory

##### 3.3.19 Download openssl folder

Download from: 
https://packages.msys2.org/package/mingw-w64-x86_64-openssl
Uzip the zst file

##### 3.3.20 Add the openssl install pkgconfig path to the PKG_CONFIG_PATH environment variable:

##### 3.3.21 Change the openssl.pc file
Change libdir and includedir to your correct install path
Remove the *.dll.a files,if you keep the *.dll.a files, you need additional libssl-1_1-x64.dll and libcrypto-1_1-x64.dll files

##### 3.3.22 Download libdl folder

Download from: 
https://packages.msys2.org/package/mingw-w64-x86_64-dlfcn
Uzip the zst file

##### 3.3.23 Copy the dlfcn.h include header files to mingw include directory,such as
C:\mingw64\x86_64-w64-mingw32\include

##### 3.3.24 Copy the libdl.a lib files to mingw lib directory,such as
C:\mingw64\lib

## 4. Open command prompt goto libraries.media.st2110.kahawai dir run 
first set PKGCONFIG path:
```
set PKG_CONFIG_PATH=c:\dpdk\lib\pkgconfig;c:\json-c\lib\pkgconfig
```
### 4.1 Build libst_dpdk
```
meson build --prefix=c:\libst_dpdk -Ddpdk_root_dir=c:\code\dpdk <--- your dpdk source code directory
ninja -C build install
```
### 4.2 Build app
```
set PKG_CONFIG_PATH=c:\dpdk\lib\pkgconfig;c:\libst_dpdk\lib\pkgconfig;c:\gtest\lib\pkgconfig;c:\openssl\lib\pkgconfig;c:\json-c\lib\pkgconfig;c:\SDL2\lib\pkgconfig
cd app
meson build
ninja -C build
```
### 4.3 Build tests
```
cd tests
meson build
ninja -C build
```