# Intel(R) Media Streaming Library for ST 2110

##### Overview

The Intel Media Streaming Library is a solution based on DPDK prepared for transmitting and receiving raw video. Raw video RTP (real time protocol) is based on the SMPTE ST 2110 standard. This solution is able to process high quality, full HD streams, without encoding. Connections with "no compression" and high speed network packets processing (utilizing DPDK) provides high quality and low latency transmission.

###### Overall Architecture

<div align="center">

<img src="/DPDKMediaStreamer/documentation/overallArchitecture.png" align="center" alt="overall architecture">
</div>

##### Use cases

Professional Audio/Video studios and data centers where high quality and low latency transmissions are necessary.

## Features

* Transmit/Receive Raw Video Frames in HD-SDI format (Pixel format: YUV 4:2:2 10bit)
* Handle standard TCP/IP stack packets
* Support for 10/25/40 Gb/s Gigabit Ethernet speeds
* Network communication (unicast and multicast) based on DPDK (supported version 21.05)
* Time synchronization using PTP (Precision Time Protocol)

## Hardware Considerations

1. NIC

For DPDK generic RTE_FLOW compatbility upgrade

* Fortville NIC's NVM to v8.00
* Columbiaville NIC's NVM to v8.30

NOTE: https://downloadcenter.intel.com/download/22283/Intel-Ethernet-Adapter-Complete-Driver-Pack

2. CPU
* For best performance with this library, we strongly recommend that Columbiaville NICs (with 100gbe capability) be used in Intel® 2nd Generation or higher Xeon® Scalable Processors.



3. MEMORY

* 8GB or more of ECC memory, populated in all memory channel.


## Setting up the environment

#### BIOS Setting

* CPU C-state Disabled
* CPU P-state Enabled
* Turbo Boost Enabled (optional)
* Hyperthread Enabled (optional)
* PCIe slot lane bifurcation Enabled (optional)
* PCIe gen 4.0 (prefferd)

#### OS
- Tested on: Ubuntu 20.04 LTS with Linux kernel 5.4

##### 1. Edit /etc/default/grub file
- on 1 NUMA, update entry to `GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=2 nomodeset isolcpus=[desired library CPU] rcu_nocbs=[desired library CPU] nohz_full=[desired library CPU] irqaffinity=[OS cores or non library CPU]"`
- on 2 NUMA, update entry to `GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=4 nomodeset isolcpus=[desired library CPU] rcu_nocbs=[desired library CPU] nohz_full=[desired library CPU] irqaffinity=[OS cores or non library CPU]"`
- on 4 NUMA, update entry to `GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=8 nomodeset isolcpus=[desired library CPU] rcu_nocbs=[desired library CPU] nohz_full=[desired library CPU] irqaffinity=[OS cores or non library CPU]"`

##### 2. execute command in terminal
```bash
update-initramfs -u
update-grub
reboot
```

##### 2.1 Performance Settings (mandatory)

Kahawai library requires CPU cores to be configured in

* high performance mode
* isolation using ISOL CPU
* populated with memory close to NIC

###### 2.1.1 Estimation on Library cores

CPU threads dsitribution based on sessions count for various essence

| Gbp/s | Video | Audio | Ancillary | TX | RX | Dual
| --- | --- | --- | --- | --- | --- | --- 
| 10 | 1 | 0 | 0 | 5 | 4 | 6
| 10 | 2 | 1 | 1 | 7 | 6 | 10
| 25 | 1 | 0 | 0 | 5 | 4 | 6
| 25 | 7 | 1 | 1 | 7 | 7 | 11
| 40 | 1 | 0 | 0 | 5 | 4 | 6
| 40 | 13 | 1 | 1 | 7 | 9 | 13
| 100 | 1 | 0 | 0 | 6 | 4 | 7
| 100 | 32 | 1 | 1 | 10 | 13 | 20
| 100 | 1 | 20 | 1 | 8 | 6 | 11 
| 100 | 1 | 1 | 20 | 8 | 6 | 11
| 100 | 20 | 20 | 20 | 9 | 13 | 19 

NOTE:
1. 100Gbps NIC has 2 threads for Audio and 2 Ancilary.
2. only one port in one NIC is used
3. session format used for validation are
 - video **1080p59**
 - audio **PCM stereo 24bit DEPTH**
 - ancillary **192 bytes text file**


###### 2.1.2 Power governer setting

Library cores can be configured with `performance` mode by

- on 1 NUMA: `echo performance | tee /sys/devices/system/cpu/cpu[library cores]/cpufreq/scaling_governor` on 1 NUMA
- on 2 NUMA: `echo performance | tee /sys/devices/system/cpu/cpu[library cores]/cpufreq/scaling_governor` on 2 NUMA
- on 4 NUMA: `echo performance | tee /sys/devices/system/cpu/cpu[library cores]/cpufreq/scaling_governor` on 4 NUMA


On 2 NUMA systems, use `lscpu` or `cpupower monitor` or `[dpdk root folder]/usertools/cpu_layout.py` to identify cores per NUMA which should run library threads. Execute in terminal
```bash
echo performance | tee /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu6/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu21/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu22/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu23/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu24/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu25/cpufreq/scaling_governor
echo performance | tee /sys/devices/system/cpu/cpu26/cpufreq/scaling_governor

```

##### 2.2 Performance Settings (optional)
- Generic settings
```bash
sysctl -w vm.zone_reclaim_mode=0
sysctl -w vm.swappiness=0
echo -1 > /proc/sys/kernel/sched_rt_period_us
echo -1 > /proc/sys/kernel/sched_rt_runtime_us
echo 10 > /proc/sys/vm/stat_interval
echo 0 > /proc/sys/kernel/watchdog_thresh
echo 0 > /proc/sys/kernel/watchdog
echo 0 > /proc/sys/kernel/nmi_watchdog
```
- Ensure NIC is enabled with full PCIe link by `lspci -vvvs [PCIe B:D:F of desired NIC] | grep Lnk`

- swap disk is disabled via `swapoff -a`


##### 3. Edit /etc/fstab file
```bash
vim /etc/fstab
```
##### 4. Add the following line
```bash
nodev	/mnt/huge_1GB	hugetlbfs	pagesize=1GB 0	0
```
##### 5. Update your software:
```bash
sudo apt update
sudo apt upgrade
```
##### 6. Install software:
```bash
sudo add-apt-repository "deb http://us.archive.ubuntu.com/ubuntu/ saucy universe multiverse"
sudo add-apt-repository "deb http://us.archive.ubuntu.com/ubuntu/ saucy-updates universe multiverse"

sudo apt install -y gcc cmake python python3 python3-pip libnuma-dev pkg-config libsdl2-dev libgtest-dev
sudo pip3 install meson ninja pyelftools
```
##### 7. Enable UIO driver:
```bash
modprobe uio
```
##### 8. Configure DPDK:
###### 8.1 Preparation of sources

###### 8.1.1 DPDK 21.05
```bash
wget http://fast.dpdk.org/rel/dpdk-21.05.tar.xz
tar xvf dpdk-21.05.tar.xz
cd dpdk-21.05
git clone http://dpdk.org/git/dpdk-kmods
```
Note: $dpdk_media_streamer points to source code of ST2110 DPDK Media Streamer obtained in [ST2110 DPDK Media Streaming library section](#MediaStreamingLibrary).
```bash
patch -p1 < $dpdk_media_streamer/DPDKMediaStreamer/patches/dpdk-$dpdk_ver/0001-net-ice-enable-1588-timesync-API-POC.patch
patch -p1 < $dpdk_media_streamer/DPDKMediaStreamer/patches/dpdk-$dpdk_ver/0004-net-ice-fix-L4-packets-not-work-issue.patch
patch -p1 < $dpdk_media_streamer/DPDKMediaStreamer/patches/dpdk-$dpdk_ver/0001-temp-fix-to-enable-multicast-rx-on-ice.patch
```

###### 8.2 Configure DPDK library features

###### 8.2.1 Set menson option in <NameOfBaseDir>/meson_options.txt
```bash
enable_kmods => true
use_hpet => true
```
###### 8.2.2 Add new build options in <NameOfBaseDir>/meson_options.txt
```bash
option('support_ieee1588', type: 'boolean', value: true,
	description: 'support IEEE1588(PTP) ')
```
###### 8.2.3 Add configuration to  <NameOfBaseDir>/config/meson.build arround line 257 (prefered)
```bash
dpdk_conf.set('RTE_LIBRTE_IEEE1588', get_option('support_ieee1588'))
```
###### 8.2.4 Build and install
```bash
cp -r ./dpdk-kmods/linux/igb_uio ./kernel/linux/
```
 - add ./kernel/linux/meson.build as `subdirs = ['kni', 'igb_uio']`
 - create a file of meson.build in ./kernel/linux/igb_uio/
```bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

mkfile = custom_target('igb_uio_makefile',
        output: 'Makefile',
        command: ['touch', '@OUTPUT@'])

custom_target('igb_uio',
        input: ['igb_uio.c', 'Kbuild'],
        output: 'igb_uio.ko',
        command: ['make', '-C', kernel_build_dir,
                'M=' + meson.current_build_dir(),
                'src=' + meson.current_source_dir(),
                'EXTRA_CFLAGS=-I' + meson.current_source_dir() +
                        '/../../../lib/librte_eal/include',
                'modules'],
        depends: mkfile,
        install: true,
        install_dir: kernel_install_dir,
        build_by_default: get_option('enable_kmods'))
```
 - build and install dpdk
```bash
meson build
ninja -C build
cd build
sudo ninja install
sudo ldconfig
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
cd ..
```
Note: If you are having difficulties building DPDK due to missing libraries (e.g. libml5.so is not found), try to build DPDK with a minimal network driver.
Use the following command instead of meson build:
```bash
meson -Ddisable_drivers=net/af_packet,net/ark,net/atlantic,net/avp,net/axgbe,net/bnx2x,net/bnxt,net/cxgbe,net/dpaa,net/dpaa2,net/e1000,net/ena,net/enetc,net/enic,net/failsafe,net/fm10k,net/hinic,net/hns3,net/igc,net/ionic,net/liquidio,net/memif,net/mlx4,mlx5,net/netvsc,net/nfp,net/octeontx,net/octeontx2,net/octeontx_ep,net/pcap,net/pfe,net/qede,net/sfc,net/softnic,net/tap,net/thunderx,net/txgbe,net/vdev_netvsc,net/vhost,net/virtio,net/vmxnet3,net/ixgbe,net/mlx5 build 
```

###### 8.3 Remarks
To uninstall the DPDK package:
```bash
cd  <NameOfOutDir>
sudo ninja uninstall
```

## Configure the Intel ST 2110 Media Streaming Library
<a name="MediaStreamingLibrary"></a>
### 1. Get resources
Clone the Intel ST 2110 Media Streaming Library repo
```bash
git clone https://github.com/OpenVisualCloud/ST2110-Media-Streaming-Library.git
cd ST2110-Media-Streaming-Library
```

### 2. Build with installation

##### 2.1 Build library
```bash
cd <MediaStreamerDir>
meson <NameOfLibOutDir>
cd <NameOfLibOutDir>
ninja
sudo ninja install
```
##### 2.2 Build sample applications
```bash
cd <MediaStreamerDir>/app
meson <NameOfAppOutDir>
cd <NameOfAppOutDir>
ninja
```

### 3. Build without installation

##### 3.1 Build library
```bash
cd <MediaStreamerDir>
meson <NameOfLibOutDir>
cd <NameOfLibOutDir>
ninja
```
##### 3.2 Build sample applications
```bash
cd <MediaStreamerDir>/app
meson <NameOfAppOutDir> -Duselocalmslib=true
cd <NameOfAppOutDir>
ninja
```

### 4. Run

#### Prerequisites
A source IP address is required to setup up the communication between transmitter and receiver. Please confirm the IP is correctly assigned.
In case no DHCP exists, it can be manually set by:
```bash
ip addr add <IP> dev <eth_dev>
```

#### 4.1 Unicast
Unicast communication is based on one transmitter and one receiver. It's necessary to define the addresses of both the transmitter and receiver.

<div align="center">

<img src="/DPDKMediaStreamer/documentation/unicastArchitecture.png" align="center" alt="unicast architecture">
</div>

###### Transmitter (Host 1)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_tx_ip <destination IP address of receiver> --app_scid 2 -f i1080p59 --p_tx --
```
###### Receiver (Host 2)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip <destination IP address of transmitter> --app_scid 2 -f i1080p59 --p_rx --
```

***Note: The receiver is expected to be started first, since the transmiter needs the receiver's ARP reply before it can send video/audio data packets***
Option "--app_scid" is used to refer to the start core id for application. It is strongly recommended to use this option in order to set application and library in same NUMA node.
If this is not set, the Kahawai library will scan avaiable cores for application from core ID 0.

#### 4.2 Multicast
Multicast communication is based on one (or more, but not in basic usage) transmitter and many receivers. It's necessary to define the IP address of the multicast group. Defining source IP address(es) is not necessary in the basic usage.

***Note: For proper use of the library when using multicast, hosts should be connected via a switch enabled with an IGMP SNOOPING option***

<div align="center">

<img src="/DPDKMediaStreamer/documentation/multicastArchitecture.png" align="center" alt="multicast architecture">
</div>

###### Transmitter (Host 1)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_tx_ip 239.0.0.10 --app_scid 2 -f i1080p59 --p_tx --
```
###### Receiver (Host 2)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip 239.0.0.10 --app_scid 2 -f i1080p59 --p_rx --
```
###### Receiver (Host 3)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip 239.0.0.11 --app_scid 2 -f i1080p59 --p_rx --
```
***Host 3 will not receive media content sent by Host 1 because it joined a different multicast group than Host 1***

###### Receiver (Host 4)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip 239.0.0.10 --app_scid 2 -f i1080p59 --p_rx --
```
#### 4.3 Redundant Path
For using the redundant path feature, two sets of port and IP pairs have to be provided; one set for the primary path and another set for the redundant path.
Option "--app_scid" is highly recommended.

###### Transmitter (Host 1)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --r_port <redundant PCI device address> --p_tx_ip <destination IP of receiver> --r_tx_ip <destination IP of receiver redundant path> -f i1080p59 --p_tx --r_tx --
```
###### Receiver (Host 2)
```bash
${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --r_port <redundant PCI device address> --p_rx_ip <destination IP of transmitter> --r_rx_ip <destination IP of transmitter redundant path> -f i1080p59 --p_rx --r_rx --
```
#### 4.4 Session Counts

###### Video Session Counts
Identify maximum video session counts based on NIC speed by:
 - Getting the speed of desired port via `ethtool [Kernel NIC] | grep Speed`
 - Looking up the table for session count based on the NIC speed.

Gbp/s | 1080p29 | 1080p59
--- | --- | ---
 10 |  4  | 2
 25 | 10  | 5
 40 | 27  | 11
100 | 64  | 32

###### Video Session Counts For Redundant path
If we use two ports in One NIC for transfer, the max session count is as below table

Notice that two ports in two NICs for transfer in Redundant path is not verified

Gbp/s | 1080p29 | 1080p59
--- | --- | ---
 10 |  2  | 1
 25 | 7  | 3
 40 | 13  | 6
100 | 32  | 16

###### Specifying Session Counts
Depending on your NIC speed and video content, use the above table to determine the appropriate session counts. The s_count, s30_count, and s40_count parameters will define session counts for ST2110-20 video, ST2110-30 audio, and ST2110-40 ancillary transimssion streams respectively. In low core count scenerios, use 1-3 video sessions and 0 session count for audio and/or ancillary to avoid unnecessary core allocations. If you set either --s30_count and/or --s40_count to 0, it will disable S30 and/or S40 respectively.

In the absense of these parameters, these defaults will be used, which will be suitable when running on low core count hardware with low NIC speeds:
s_count 1
s30_count 0
s40_count 0

#### 4.5 Available parameters
```bash
-h                                           : print help info (optional)
-v                                           : print versions info (optional)
--p_tx_ip <IP addr>                          : destination TX IP address for primary port(required when --p_tx is used)
--r_tx_ip <IP addr>                          : destination TX IP address of redundant port (required  when --r_tx is used)
--p_rx_ip <IP addr>                          : destination RX IP address for primary port(required when --p_rx is used)
--r_rx_ip <IP addr>                          : destination RX IP address for redundant port(required  when --r_rx is used)
--ebu                                        : enable EBU compatibility with standard ST 2110 logs
--port <UDP port>                            : base port from which to iterate sessions port IDs (optional)
--p_tx                                       : run transmit from primary port (required)
--p_rx                                       : run receive from primary port
--r_tx                                       : run transmit from redundant port
--r_rx                                       : run receive from redundant port

--format <fmt string>                        : select frame format e.g. a1080i50 = aya 1080 interlaced 50fps (optional)
                                                : e.g. i720p29  = intel 720 progressive 29.97fps
                                                : e.g. i1080p59 = intel 1080 progressive 59.94fps
                                                : e.g. i2160p59 = intel 2160 progressive 59.94fps
                                                : e.g. i1080i29 = intel 1080 interlaced 29.97fps
                                                : e.g. a1080p59 = aya 1080 progressive 59.94fps
--audioFrame  <Audio frame size>             : size of Audio frame in bytes, user provides based on frequency, channel count and bit depth for desired duration of audio samples (e.g. 1ms)
                                                : e.g. for 48kHz sampling frequency, 2 channels, bit depth 16bites value is 192
--s_count <number of sessions>	             : number of video sessions (optional)
--s30_count <number of sessions>             : number of audio sessions (optional)
--s40_count <number of sessions>             : number of ancillary sessions (optional) 
--app_scid <core id>                         : application start core id
--lib_cid <core id>                          : cores on which library thread needs to run e.g. 1,2,3,4
--enqueue_threads <number of enqueue thread> : number of enqueue threads (optional)
--p_port <PCI device address>	             : primary interface PCI device address (required)
--r_port <PCI device address>	             : redundant interface PCI device address (required for redundant path)
--ptp <hhhhhh.hhhh.hhhhhh>                   : master clock id
--log_level <user,level<info/debug/error>>   : enable additional logs
--videoFile <filename>                       : specifying the path to send video file (Currently application is working with YUV format 422 10bit BE only)
--audioFile <filename>                       : specifying the path to send audio file
--ancFile <filename>                         : specifying the path to send anciliary file
--pacing <pause/tsc>                         : the pacing type (optional)
```

### 5. Test
The Intel(R) Media Streaming Library for ST 2110 contains a separate project for unit and module tests. The test environment is based on the Google Testing Framework.

#### 5.1 Build tests with installed Intel(R) Media Streaming Library for ST 2110 library
```bash
cd <MediaStreamerDir>/tests
meson <NameOfTestOutDir>
cd <NameOfTestOutDir>
ninja
```

#### 5.2 Build tests without installed Intel(R) Media Streaming Library for ST 2110 library
```bash
cd <MediaStreamerDir>/tests
meson <NameOfTestOutDir> -Duselocalmslib=true
cd <NameOfTestOutDir>
ninja
```
