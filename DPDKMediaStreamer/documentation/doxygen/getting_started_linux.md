@page gs_linux Getting Started with Linux


@section gs_linux_sec1 1. System Requirements

@subsection gs_linux_subsec1_1 1.1 Hardware Requirements

Recommended NIC: 
* Intel X7** series (Fortville) (10Gbps, 25Gbps, 40Gbps)
* Intel E8** series (Columbiaville) (10Gbps, 25Gbps, 100Gbps)

<b>NOTE:</b><br>
<b>   For DPDK generic RTE_FLOW compatbility upgrade </b><br>
<b>        * Fortville NIC's NVM to v8.00 </b><br>
<b>        * Columbiaville NIC's NVM to v8.30 </b><br>
<b>    https://downloadcenter.intel.com/download/22283/Intel-Ethernet-Adapter-Complete-Driver-Pack</b>


CPU:
Intel Xeon processor.
    * Haswell family
    * Broadwell family
    * Skylake family
    * Icelake family


MEMORY:
* RAM: 64GB all memory channel
* Storage: SSD, NvME 240GB


8GB or more of ECC memory, populated in all memory channels.

@subsection gs_linux_subsec1_2 1.2 Software Requirements

BIOS settings:

* CPU C-state Disabled
* CPU P-state Enabled
* Turbo Boost Enabled (optional)
* Hyperthreading Enabled (optional)
* PCIe slot lane bifurcation Enabled (optional)
* PCIe gen 4.0 (prefferd)

Operating system: 

* Ubuntu 20.04 LTS with Linux kernel 5.4

@section gs_linux_sec2 2. Preparing Environment

@subsection gs_linux_subsec2_1 2.1 GRUB Preparation

#### 1. Edit /etc/default/grub file

* on 1 NUMA, update entry to GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=2 nomodeset isolcpus=[desired library CPU] rcu_nocbs=[desired library CPU] nohz_full=[desired library CPU] irqaffinity=[OS cores or non library CPU]"

* on 2 NUMA, update entry to GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=4 nomodeset isolcpus=[desired library CPU] rcu_nocbs=[desired library CPU] nohz_full=[desired library CPU] irqaffinity=[OS cores or non library CPU]"

* on 4 NUMA, update entry to GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=8 nomodeset isolcpus=[desired library CPU] rcu_nocbs=[desired library CPU] nohz_full=[desired library CPU] irqaffinity=[OS cores or non library CPU]"



#### 2. Execute command in terminal

    update-initramfs
    update-grub
    reboot

@subsection gs_linux_subsec2_2 2.2 Performance Setting

#### Mandatory part:

Kahawai library requires CPU cores to be configured in

* high performance
* ISOL CPU
* populated with memory close to NIC


#### 1. Estimation on Library cores

Gbp/s | Min CPU threads for TX | Min CPU threads for RX
--- | --- | ---
10 | 5 | 4
25 | 6 | 5
40 | 7 | 6


#### 2. Power governer setting

Library cores can be configured with performance mode by

* on 1 NUMA: 


        echo performance | tee /sys/devices/system/cpu/cpu[library cores]/cpufreq/scaling_governor
* on 2 NUMA: 


        echo performance | tee /sys/devices/system/cpu/cpu[library cores]/cpufreq/scaling_governor
* on 4 NUMA: 


        echo performance | tee /sys/devices/system/cpu/cpu[library cores]/cpufreq/scaling_governor

On 2 NUMA systems, use lscpu or cpupower monitor or [dpdk root folder]/usertools/cpu_layout.py to identify cores per NUMA which should run library threads. Execute in terminal

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


#### Optional part:

Generic settings

    sysctl -w vm.zone_reclaim_mode=0
    sysctl -w vm.swappiness=0
    echo -1 > /proc/sys/kernel/sched_rt_period_us
    echo -1 > /proc/sys/kernel/sched_rt_runtime_us
    echo 10 > /proc/sys/vm/stat_interval
    echo 0 > /proc/sys/kernel/watchdog_thresh
    echo 0 > /proc/sys/kernel/watchdog
    echo 0 > /proc/sys/kernel/nmi_watchdog


Ensure NIC is enabled with full PCIe link:

    lspci -vvvs [PCIe B:D:F of desired NIC] | grep Lnk


Swap disk is disabled:

    swapoff -a


@subsection gs_linux_subsec2_3 2.3 HugePage Setting 

#### 1. Edit /etc/fstab file

    vim /etc/fstab

#### 2. Add the following line

    nodev	/mnt/huge_1GB	hugetlbfs	pagesize=1GB 0	0

@subsection gs_linux_subsec2_4 2.4 Additional software installation

#### 1. Update your software:

    sudo apt update
    sudo apt upgrade

#### 2. Install software:

    sudo add-apt-repository "deb http://us.archive.ubuntu.com/ubuntu/ saucy universe multiverse"
    sudo add-apt-repository "deb http://us.archive.ubuntu.com/ubuntu/ saucy-updates universe multiverse"

    sudo apt install -y gcc cmake python python3 python3-pip libnuma-dev pkg-config libsdl2-dev libgtest-dev
    sudo pip3 install meson ninja pyelftools

3.Enable UIO driver:
    
    modprobe uio

@subsection gs_linux_subsec2_5 2.5 DPDK preparation

#### 1. Preparation of sources

    wget http://fast.dpdk.org/rel/dpdk-21.05.tar.xz
    tar xvf dpdk-21.05.tar.xz
    cd dpdk-21.05
    git clone http://dpdk.org/git/dpdk-kmods

Note: $dpdk_media_streamer points to source code of ST2110 DPDK Media Streamer obtained in [ST2110 DPDK Media Streaming library section](#MediaStreamingLibrary)

        patch -p1 < $dpdk_media_streamer/CI/dpdk-$dpdk_ver/0001-net-ice-enable-1588-timesync-API-POC.patch
        patch -p1 < $dpdk_media_streamer/CI/dpdk-$dpdk_ver/0004-net-ice-fix-L4-packets-not-work-issue.patch
        patch -p1 < $dpdk_media_streamer/CI/dpdk-$dpdk_ver/0001-temp-fix-to-enable-multicast-rx-on-ice.patch


#### 2. Set menson option in < NameOfBaseDir >/meson_options.txt

    enable_kmods set to true
    use_hpet set to true

#### 3. Add new build options in < NameOfBaseDir >/meson_options.txt

    option('support_ieee1588', type: 'boolean', value: true,
            description: 'support IEEE1588(PTP) ')

#### 4. Add configuration to  < NameOfBaseDir >/config/meson.build arround line 257 (prefered)

    dpdk_conf.set('RTE_LIBRTE_IEEE1588', get_option('support_ieee1588'))

#### 5. Build and install


    cp -r ./dpdk-kmods/linux/igb_uio ./kernel/linux/

* add ./kernel/linux/meson.build as `subdirs = ['kni', 'igb_uio']`
* create a file of meson.build in ./kernel/linux/igb_uio/


    # SPDX-License-Identifier: BSD-3-Clause
    # Copyright(c) 2017 Intel Corporation

    mkfile = custom_target('igb_uio_makefile',
            output: 'Makefile',
            command: ['touch', '@OUTPUT@'])

    custom_target('igb_uio',
            input: ['igb_uio.c', 'Kbuild'],
            output: 'igb_uio.ko',
            command: ['make', '-C', kernel_dir + '/build',
                    'M=' + meson.current_build_dir(),
                    'src=' + meson.current_source_dir(),
                    'EXTRA_CFLAGS=-I' + meson.current_source_dir() +
                            '/../../../lib/librte_eal/include',
                    'modules'],
            depends: mkfile,
            install: true,
            install_dir: kernel_dir + '/extra/dpdk',
            build_by_default: get_option('enable_kmods'))

* build and install dpdk


    meson build
    ninja -C build
    cd build
    sudo ninja install
    sudo ldconfig
    pkg-config --cflags libdpdk
    pkg-config --libs libdpdk
    cd ..

Note: If you are having difficulties building DPDK due to missing libraries (e.g. libml5.so is not found), try to build DPDK with a minimal network driver.
Use the following command instead of meson build:

    meson -Ddisable_drivers=net/af_packet,net/ark,net/atlantic,net/avp,net/axgbe,net/bnx2x,net/bnxt,net/cxgbe,net/dpaa,net/dpaa2,net/e1000,net/ena,net/enetc,net/enic,net/failsafe,net/fm10k,net/hinic,net/hns3,net/igc,net/ionic,net/liquidio,net/memif,net/mlx4,mlx5,net/netvsc,net/nfp,net/octeontx,net/octeontx2,net/octeontx_ep,net/pcap,net/pfe,net/qede,net/sfc,net/softnic,net/tap,net/thunderx,net/txgbe,net/vdev_netvsc,net/vhost,net/virtio,net/vmxnet3,net/ixgbe,net/mlx5 build 


#### 6. Remarks

To uninstall the DPDK package:

    cd  <NameOfOutDir>
    sudo ninja uninstall



@section gs_linux_sec3 3. Building and installation

#### 1. Get resources

Clone the Intel ST 2110 Media Streaming Library repo:

    git clone https://gitlab.devtools.intel.com/VEI/dpdk_media_streamer.git
    cd dpdk_media_streamer


#### 2. Build with installation

##### 2.1 Build library

    cd <MediaStreamerDir>
    meson <NameOfLibOutDir>
    cd <NameOfLibOutDir>
    ninja
    sudo ninja install

##### 2.2 Build sample applications

    cd <MediaStreamerDir>/app
    meson <NameOfAppOutDir>
    cd <NameOfAppOutDir>
    ninja

#### 3. Build without installation

##### 3.1 Build library

    cd <MediaStreamerDir>
    meson <NameOfLibOutDir>
    cd <NameOfLibOutDir>
    ninja

##### 3.2 Build sample applications

    cd <MediaStreamerDir>/app
    meson <NameOfAppOutDir> -Duselocalmslib=true
    cd <NameOfAppOutDir>
    ninja



@section gs_linux_sec4 4. Running Sample Application

#### Prerequisites
A source IP address is required to setup up the communication between transmitter and receiver. Please confirm the IP is correctly assigned.
In case no DHCP exists, it can be manually set by:

    ip addr add <IP> dev <eth_dev>

#### 1 Unicast
Unicast communication is based on one transmitter and one receiver. It's necessary to define addresses of both the transmitter and receiver.

\image html unicastArchitecture.png

##### Transmitter (Host 1)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_tx_ip <destination IP address of receiver> --app_scid 2 -f i1080p59 --p_tx --

##### Receiver (Host 2)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip <destination IP address of transmitter> --app_scid 2 -f i1080p59 --p_rx --


***Note: The receiver is expected to be started first, since the transmiter needs the receiver's ARP reply before it can send video/audio data packets***
Option "--app_scid" is used to refer to the start core id for application. Strongly recommend use this option to set application and library in same NUMA node.
If this is not set, Kahawai library will scan avaiable cores for application from core ID 0.

#### 2 Multicast
Multicast communication is based on one (or more, but not in basic usage) transmitter and many receivers. It's necessary to define the IP address of the multicast group. Defining source IP address(es) is not necessary in the basic usage.

***Note: For proper use of the library when using multicast, hosts should be connected via a switch enabled with an IGMP SNOOPING option***

\image html multicastArchitecture.png

##### Transmitter (Host 1)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_tx_ip 239.0.0.10 --app_scid 2 -f i1080p59 --p_tx --

##### Receiver (Host 2)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip 239.0.0.10 --app_scid 2 -f i1080p59 --p_rx --

##### Receiver (Host 3)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip 239.0.0.11 --app_scid 2 -f i1080p59 --p_rx --

***Host 3 will not receive media content sent by Host 1 because it joined a different multicast group than Host 1***

##### Receiver (Host 4)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --p_rx_ip 239.0.0.10 --app_scid 2 -f i1080p59 --p_rx --

#### 3 Redundant Path
For using the redundant path feature, two sets of port and IP pairs have to be provided, one set for the primary path and another set for the redundant path.
Option "--app_scid" is highly recommended.

##### Transmitter (Host 1)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --r_port <redundant PCI device address> --p_tx_ip <destination IP of receiver> --r_tx_ip <destination IP of receiver redundant path> -f i1080p59 --p_tx --r_tx --

##### Receiver (Host 2)

    ${NameOfAppOutDir}/rxtx_app/RxTxApp --p_port <primary PCI device address> --r_port <redundant PCI device address> --p_rx_ip <destination IP of transmitter> --r_rx_ip <destination IP of transmitter redundant path> -f i1080p59 --p_rx --r_rx --

@section gs_linux_sec5 5. Session count

##### Video Session Counts
Identify maximum video session counts based on NIC speed by
 - Get the speed of desired port via `ethtool [Kernel NIC] | grep Speed`
 - Look up the table for session count based on the NIC speed.

Gbp/s | 1080p29 | 1080p59
--- | --- | ---
 10 |  4  | 2
 25 | 10  | 5
 40 | 27  | 11
 50 | 28  | 14

##### Specifying Session Counts
Depending on your NIC speed and video content, use the above table to determine the appropriate session counts. The s_count, s30_count, and s40_count parameters will define session counts for ST2110-20 video, ST2110-30 audio, and ST2110-40 ancillary transimssion streams respectively. In low core count scenerios, Use 1-3 video sessions  and 0 session count for audio and/or ancillary to avoid unnecessary core allocations. If you either --s30_count and/or --s40_count to 0, it will disable S30 and/or S40 respectively.

In the absense of these parameters, these defaults will be used, which will be suitable when running on low core count hardware with low NIC speeds:
s_count 1
s30_count 0
s40_count 0

@section gs_linux_sec6 6.  Available parameters

    -h                                           : print help info (optional)
    -v                                           : print versions info (optional)
    --p_tx_ip <IP addr>                          : destination TX IP address for primary port(required when p_tx = 1)
    --r_tx_ip <IP addr>                          : destination TX IP address of redundant port (required  when r_tx = 1)
    --p_rx_ip <IP addr>                          : destination RX IP address for primary port(required when p_rx = 1)
    --r_rx_ip <IP addr>                          : destination RX IP address for redundant port(required  when r_rx = 1)
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
    --audioFrame  <Audio frame size>             : Size of Audio frame in bytes, user provides based on frequency, channel count and bit depth for desired duration of audio samples (e.g. 1ms)
                                                    : e.g. for 48kHz sampling frequency, 2 channels, bit depth 16bites value is 192
    --s_count <number of sessions>	             : number of video sessions (optional)
    --s30_count <number of sessions>             : number of audio sessions (optional)
    --s40_count <number of sessions>             : number of ancillary sessions (optional) 
    --app_scid <core id>                         : application start core id
    --lib_scid <core id>                         : library core id e.g. 1,2,3,4
    --enqueue_threads <number of enqueue thread> : number of enqueue threads (optional)
    --p_port <PCI device address>	             : primary interface PCI device address (required)
    --r_port <PCI device address>	             : redundant interface PCI device address (required for redundant path)
    --ptp <hhhhhh.hhhh.hhhhhh>                   : master clock id
    --log_level <user,level<info/debug/error>>   : enable additional logs
    --videoFile <filename>                       : specyfying the path to send video file (Currently application is working with YUV format 422 10bit BE only)
    --audioFile <filename>                       : specyfying the path to send audio file
    --ancFile <filename>                         : specyfying the path to send anciliary file
    --pacing <pause/tsc>                         : the pacing type (optional)




