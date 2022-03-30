# Intel(R) Media Streaming Library for ST 2110 compilation and build on Windows OS

## 1. Introduction

This document contains instructions for installing and configuring the Intel(R) Media Streaming Library for ST 2110 
for Windows Operation System.
This document describes how to compile and run a Intel(R) Media Streaming Library for ST 2110 and sample application 
in a Windows OS environment. 

All the steps below related to the DPDK configuration instructions on Windows are from the website:
https://doc.dpdk.org/guides/windows_gsg/intro.html

### 1.1 OS

- Tested on: Windows Server 2019

## 2. Preparation of the environment 

To prepare the environment, follow the instructions on the DPDk windows page.
The recommended options is:
"3.3. Option 2. MinGW-w64 Toolchain and 3.6.1. Option 1. Native Build on Windows"

### 2.1 PATH environmental variable in Windows
After downloading and installing Meson and Ninja you must add them to the PATH environmental variable in Windows. Search for edit the system environment variables and run the program. Click on Environment Variables... button and then in the upper section click on Path variable and click edit. Then add it to your environment variable by clicking new and writing the path down to them just like in the example below.
for example:

C:Program Files\Meson\
C:\Ninja\ninja.exe


## 3. Installation and configuration of DPDK
First install compile tools mingw64, suggest to install to c:\mingw64
https://sourceforge.net/projects/mingw-w64/files/Toolchains%20targetting%20Win64/Personal%20Builds/mingw-builds/8.1.0/threads-posix/sjlj/x86_64-8.1.0-release-posix-sjlj-rt_v6-rev0.7z

### 3.1 Preparation of sources

##### 3.1.1 Download DPDK sources from:

https://fast.dpdk.org/rel/dpdk-21.08.tar.xz

##### 3.1.2 Unpack the sources
To unpack you can use 7-zip:

•	Right-click the file

•	Select 7-zip -> Extract Here / Extract to

##### 3.1.3 Patch dpdk-21.08 with custom patches
Note: $dpdk_media_streamer points to source code of ST2110 DPDK Media Streamer obtained in [ST2110 DPDK Media Streaming library section](#MediaStreamingLibrary).
If there is a CLV card given the patch are needed.

Run the following commands using gitbash:
Execute below command 
```bash
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0001-update-ice-1588-timesync-based-on-21.08.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0002-temp-fix-to-enable-multicast-rx-on-ice.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0003-fix-L4-packet-not-work.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0004-eanble-PF-ICE-rate-limit.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0005-update-VF-rate-limit.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0006-net-iavf-add-capability-for-runtime-rx-tx-queue-setu.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0007-build-enable-IEEE1588-PTP-option.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0008-enable-rx-timestamp-and-fix-performance-issue.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0009-net-ice-support-max-burst-size-configuration.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/0010-Add-init-time-to-sync-PHY-timer-with-primary-timer.patch
Then apply windows platform dpdk patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0001-Add-stack-module-support-in-windows-version.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0002-Add-DDP-package-load-support-in-windows.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0003-Change-list-remove-and-add-position-to-avoid-race-co.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0004-Mingw-compiler-do-have-same-implementation.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0005-Mingw-do-have-pthread-implementation-change-to-adapt.patch
patch -p1 < {path_to_repo_dir}/libraries.media.st2110.kahawai/patches/dpdk/{dpdk_dir_ver}/windows/0006-Bind-thread-to-dedicate-core-in-windows-version.patch
```

##### 3.1.4 Add new build options in /meson_options.txt

```bash
option('support_ieee1588', type: 'boolean', value: true,
	description: 'support IEEE1588(PTP) ')
```

##### 3.1.5 Add configuration to  /config/meson.build arround line 257 (prefered)

```bash
dpdk_conf.set('RTE_LIBRTE_IEEE1588', get_option('support_ieee1588'))
```

### 3.2 Build and install DPDK
Execute the commands below in cmd:

```bash
cd  C:\path_to\dpdk-21.08
meson build
ninja -C build install
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

```bash
C:\path_to\pkg-config_0.26-1\bin\
C:\path_to\glib_2.28.8-1_win32\bin\
C:\path_to\gettext-rintime_0.18.1.1-2_win32\bin\
```

##### 3.3.4 Download SDL2 folder

Download from: 
https://www.libsdl.org/download-2.0.php

##### 3.3.5 Create new environment variable:
PKG_CONFIG_PATH is correct installed and compiled package path for pkg-config searching
For example, set PKG_CONFIG_PATH=c:\dpdk\lib\pkgconfig;c:\libst_dpdk\lib\pkgconfig

##### 3.3.6 Copy the sdl2 include header files to mingw include directory,such as
C:\mingw64\x86_64-w64-mingw32\include


##### 3.3.7 Download json-c folder

Download from: 
https://packages.msys2.org/package/mingw-w64-x86_64-json-c
Uzip the zst file

##### 3.3.8 Add the json-c install pdgconfig path to the environment variable:
PKG_CONFIG_PATH

##### 3.3.9 Copy the json-c include header files to mingw include directory,such as
C:\mingw64\x86_64-w64-mingw32\include

##### 3.3.10 Download win-pcap folder

Download from: 
https://nmap.org/npcap/dist/npcap-sdk-1.11.zip
Uzip the zip file

##### 3.3.11 Copy the npcap lib files to mingw lib directory,such as
C:\mingw64\lib

##### 3.3.12 Copy the npcap include header files to mingw include directory,such as
C:\mingw64\include

##### 3.3.13 Download mman folder

Download from: 
https://github.com/alitrack/mman-win32
Uzip the zip file, compile the project using visual studio 2019

##### 3.3.14 Copy the mman lib files to mingw lib directory,such as
C:\mingw64\lib

##### 3.3.15 Copy the mman include header files to mingw include directory,such as
C:\mingw64\x86_64-w64-mingw32\include\sys


## 3.4 Grant Lock pages in memory Privilege

##### 3.4.1 Open Local Security Policy snap-in, type “secpol” in searching window


##### 3.4.2 Open Local Security Policy -> Computer Management -> Local Security Policy 
##### 3.4.3 Double click on Local Security Policy
##### 3.4.4 Click “Add User or Group”

##### 3.4.5 Add Administrator

## 3.5 Load virt2phys Driver

##### 3.5.1 Download dpdk-kmods pack from:
git://dpdk.org/dpdk-kmods
Compile the virt2phys and netuio project using visual studio 2019

##### 3.5.2 Then, execute command in cmd:

```bash
pnputil /add-driver Z:\path\to\virt2phys.inf /install
```

##### 3.5.3 Make sure that the driver was installed 

##### 3.5.4 (Optional) When there is a problem with driver installation are needed more steps:
	
•	test sign” the driver using a test certificate and then boot the Windows in “Test mode”, or

•	Use the boot time option to “Disable driver signature enforcement”

##### 3.5.6 (Optional) Additional steps for Windows Server:

•	From Device Manager, Action menu, select “Add legacy hardware”.	
	
•	It will launch the “Add Hardware Wizard”. Click “Next”

•	Select second option “Install the hardware that I manually select from a list”

•	On the next screen, “Kernel bypass” will be shown as a device class
	
•	Select it and click “Next”.

•       Click "Have Disk".

•       Find location of your virt2phys.inf driver.

•       Select it and click “Next”.

•	The previously installed drivers will now be installed for the “Virtual to physical address translator” device


#### 3.5.7 Here we just go through next and finish buttons.
#### 3.5.8 Same step as above to install the netuio driver by right clicking the ICE driver in device manager
#### 3.5.9 Copy the ICE driver related DDP file ice-1.3.26.0.pkg to the same directory as rxtxapp.exe file, can download from Intel site:
https://downloadmirror.intel.com/681886/26_6.zip
#### 3.5.10 Create the temp folder in root directory c:\

