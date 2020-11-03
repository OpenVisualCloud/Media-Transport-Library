# Intel(R) Media Streaming Library for ST 2110

##### Overview

Media Streaming Library is solution based on DPDK prepared for transmitting and receiving raw video. Raw video RTP(real time protocol) is based on the SMPTE ST 2110-21 protocol. This solution are able to process high quality, full HD stream, without encoding. Connection of "no compression" and high speed network packets processing (DPDK) gives high quality and low latency.

##### Use cases

Professional Audio/Video studios and data centers where the high quality and low latency are necessary.

##### More information 

https://wiki.ith.intel.com/display/DMS/DPDK+Media+Streamer+Home

## Features
 
* Transmit/Receive Raw Video Frames in HD-SDI format (Pixel format: YUV 4:2:2 10bit)
* Handling standard TCP/IP stack packets
* Support for 10/25/40/100 Gb/s Gigabit Ethernet
* Network communication based on DPDK (supported version 20.02)

## Changelog

### **API changes**

##### API Version 1.0.3
* Function name change from **St21ProducerRegister** to the **St21RegisterProducer**
* Function name change from **St21ConsumerRegister** to the **St21RegisterConsumer**

##### API Version 1.0.4
* Added implementation of the **St21GetSessionCount** function

##### API Version 1.0.5 
* Added implementation of the **St21SetParam** function
* Added implementation of the **St21GetParam** function
* Added implementation of the **St21GetSdp** function
* Removed **St21SendSdp** function
* Removed **St21ReceiveSdp** function

##### API Version 1.0.6
* Function parameters change at **StPtpSetClockSource**

##### API Version 1.0.7
* Added API function named **StInitLib**

##### API Version 1.0.8
* Added API function named **StPtpGetClockSource**

##### API Version 1.0.9
* Added "*Field ID*" parameter to support interlace standard

##### API Version 1.0.11
* Added new error code *ST_DEV_ERR_NOT_READY (-509)*

##### API Version 1.0.12
* App version is no longer supported by Library

##### API Version 1.0.13
* Added **st21_format_name** enum

##### API Version 1.0.14
* Changes in the **st21_prod_type** enum
* Changes in the **st21_cons_type** enum

##### API Version 1.0.15
* Removed **StInitLib** function
* Added **StGetParam** function
* Added **StSetParam** function

### **LIB changes**

##### LIB Version 1.0.5
* Added parameter **'mip'** to use multicast IP addresses

##### LIB Version 1.0.6
* Added support for 720p resolution

##### LIB Version 1.0.7
* Added parameter **'ebu'** to enable EBU logs (compatibility with standard ST 2110)

##### LIB Version 1.0.8
* Fixed **StDestroyDevice** function

##### LIB Version 1.0.9
* Parsing arguments - removed from the library and moved into the sample application
* Added parameter **'log_level'** to enable additional logs

## Set up the environment

#### Recomended OS - Ubuntu 18.04 LTS with kernel 5.3

##### 1. Edit /etc/default/grub file
```bash
vim /etc/default/grub
```
##### 2. Change following line
```bash
GRUB_CMDLINE_LINUX_DEFAULT="splash default_hugepagesz=1G hugepagesz=1G hugepages=8 nomodeset"
```
##### 3. Edit /etc/fstab file
```bash
vim /etc/fstab
```
##### 4. Add following line
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

sudo apt install -y dpdk-igb-uio-dkms gcc cmake python python3 python3-pip libnuma-dev pkg-config libsdl2*-dev
sudo pip3 install meson ninja
```
##### 7. Enable UIO driver:
```bash
modprobe uio
```
##### 8. Configure DPDK:
###### 8.1 Preparation of sources
###### 8.1.1 Using latest stable version of DPDK library from GitHub (prefered)
Clone DPDK repo
```bash
git clone https://github.com/DPDK/dpdk.git <NameOfBaseDir>
```
Checkout to v20.08 version
```bash
git checkout --no-track -b v20.08 v20.08
cd <NameOfBaseDir>
```
###### 8.1.2 Using internal copy on Intel GitLab (not recommended)
```bash
git clone -b dpdk ssh://git@gitlab.devtools.intel.com:29418/VEI/dpdk_media_streamer.git <NameOfBaseDir>
pushd <NameOfBaseDir>
git remote add dpdk.org https://dpdk.org/git/dpdk
git remote add github.com https://github.com/DPDK/dpdk.git
```
###### 8.2 Configure library features

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
###### 8.2.3 Add configuration to  <NameOfBaseDir>/config/meson.build arround line 230 (prefered)
```bash
dpdk_conf.set('RTE_LIBRTE_IEEE1588', get_option('support_ieee1588'))
```
###### 8.2.4 Build and install
```bash
meson <NameOfOutDir>
cd <NameOfOutDir>
ninja
sudo ninja install
sudo ldconfig
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
cd ..

```
###### 8.3 Remarks
Packages uninstallation:
```bash
cd  <NameOfOutDir>
sudo ninja uninstall
```

## Configure Intel ST 2110 Media Streaming Library

### 1. Get resources
Clone Intel ST 2110 Media Streaming Library repo
```bash
git clone https://gitlab.devtools.intel.com/VEI/dpdk_media_streamer.git
cd dpdk_media_streamer
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

#### 4. Run
```bash
${NameOfAppOutDir}/rx_app/RxApp --in_port <input PCI device address> --mac <destination MAC address> --ip <destination IP address> --sip <source IP address> --o_port <output PCI device address> --
```
##### Available parameters:
```bash
-h                                           : print help info (optional)
-v                                           : print versions info (optional)
--mac <MAC addr>                             : destination MAC address (required)
--ip <IP addr>                               : destination IP address (required)
--sip <IP addr>                              : source IP address (required)
--mip                                        : enable multicast flag
--ebu                                        : enable EBU compatibility with standard ST 2110 logs
--port <UDP port>                            : base port from which to iterate sessions port IDs (optional)
--rx                                         : run receive mode only (optional)
--tx                                         : run transmit mode only (optional)
--rgba                                       : input / output buffers are in rgba format
--yuv10be                                    : input / output buffers are in yuv10be format
--format <fmt string>                        : select frame format e.g. a1080i50 = aya 1080 interlaced 50fps (optional)
                                                : e.g. i720p29  = intel 720 progressive 29.97fps
                                                : e.g. i1080p59 = intel 1080 progressive 59.94fps
                                                : e.g. i2160p59 = intel 2160 progressive 59.94fps
                                                : e.g. i1080i29 = intel 1080 interlaced 29.97fps
                                                : e.g. a1080p59 = aya 1080 progressive 59.94fps
--s_count <number of sessions>	             : number of sessions (optional)
--o_port <PCI device address>	             : output interface PCI device address (required)
--in_port <PCI device address>	             : input interface PCI device address (required)
--ptp <hhhhhh.hhhh.hhhhhh>                   : master clock id
--log_level <user,level<info/debug/error>>   : enable additional logs 
```
#### 2.1 Running application using environment variables

##### Edit /etc/environment file
```bash
vim /etc/environment
```
##### Set following variables
```bash
RTE_SDK=/home/dpdk
RTE_TARGET=x86_64-native-linux-gcc
IN_PORT=0000:af:00.1               # PCI addres of NIC to recv frames
OUT_PORT=0000:af:00.0              # PCI adress of NIC to send frames
OUT_IP=192.168.0.2                 # IP addres for sender
DEST_IP=192.168.0.1                # IP addres of destination
DEST_MAC=12:34:56:ab:cd:ef         # MAC of destination
ST_NIC_RATE_GBPS=ST_NIC_RATE_SPEED_10GBPS
ST_RX='--rx'
```

```bash
${NameOfAppOutDir}/rx_app/RxApp ${ST_RX-} --in_port $IN_PORT --o_port $OUT_PORT --mac $DEST_MAC --ip $DEST_IP --sip $OUT_IP --
```
