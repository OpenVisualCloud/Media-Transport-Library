# MTL RDMA Library

## Introduction

The Media Transport Library (MTL) was developed to streamline the process of sending and receiving media over standard network interfaces, specifically focusing on the ST2110 standard.
ST2110 is crucial for transmitting high-quality, uncompressed video and audio over IP networks.
Traditionally, cameras used Serial Digital Interface (SDI) to send footage for processing.
However, with the emergence of ST2110, media data can now be encapsulated in RTP packets and transmitted via Ethernet, making the process more straightforward and less reliant on specialized hardware.
With MTL, you can transmit or receive ST2110 standard-compliant streams, eliminating the need for specialized and expensive ST2110 hardware.
This technology can also transport raw video/audio data between data center machines.

One challenge with MTL's ST2110 stack is its reliance on DPDK (Data Plane Development Kit), which demands dedicated CPU resources for network packet handling.
This is where RDMA comes into play, offering a solution to offload these tasks from the CPU.
Specifically, the Intel E810 Network Interface Card (NIC) supports RoCEv2, an RDMA implementation that uses the UDP protocol.
By leveraging RDMA and its interrupt mode, significant CPU resources can be conserved, which is particularly advantageous in cloud computing environments where resource optimization is critical.

## Enable IRDMA driver

To enable RoCEv2 support on Intel E810 NIC, first you need to make sure the irdma kernel module is installed and loaded.

```bash
lsmod | grep rdma
```
```text
irdma                 430080  0
i40e                  638976  1 irdma
ib_uverbs             196608  1 irdma
ib_core               516096  2 irdma,ib_uverbs
ice                  1081344  1 irdma
```

For some kernel version, RoCEv2 is not enabled by default.
You need to reload the module.

```bash
sudo rmmod irdma
sudo modprobe irdma roce_ena=1
```

If you are using out of tree ice driver (see [e810.md](e810.md)), you may need to install the out of tree irdma too.

Go to [irdma](https://www.intel.com/content/www/us/en/download/19632/linux-rdma-driver-for-the-e810-and-x722-intel-ethernet-controllers.html) to download the latest tarball and follow the readme to install.

## Install RDMA dependencies

```bash
sudo apt update
sudo apt install infiniband-diags rdma-core ibverbs-utils perftest
```

## Configure RDMA devices

### Configure network interfaces

For example the E810 interfaces are `ens8np0` and `ens9np0`.

```bash
sudo ip addr add 192.168.99.110/24 dev ens8np0
sudo ip addr add 192.168.99.111/24 dev ens9np0
sudo ip link set dev ens8np0 up
sudo ip link set dev ens9np0 up
sudo sysctl net.ipv4.conf.ens8np0.accept_local=1
sudo sysctl net.ipv4.conf.ens9np0.accept_local=1
```

Check the connectivity.

```bash
ping -I ens8np0 192.168.99.111
```

### Check active RDMA devices

```bash
ibv_devices
```
```text
    device                 node GUID
    ------              ----------------
    irdma0              6a05cafffec1b900
    irdma1              6a05cafffec1b901
```
```bash
ibstat
```
```text
CA 'irdma0'
        CA type:
        Number of ports: 1
        Firmware version: 1.71
        Hardware version:
        Node GUID: 0x6a05cafffec1b900
        System image GUID: 0x6a05cafffec1b900
        Port 1:
                State: Active
                Physical state: LinkUp
                Rate: 25
                Base lid: 1
                LMC: 0
                SM lid: 0
                Capability mask: 0x00050000
                Port GUID: 0x6a05cafffec1b900
                Link layer: Ethernet
CA 'irdma1'
        CA type:
        Number of ports: 1
        Firmware version: 1.71
        Hardware version:
        Node GUID: 0x6a05cafffec1b901
        System image GUID: 0x6a05cafffec1b901
        Port 1:
                State: Active
                Physical state: LinkUp
                Rate: 10 (FDR10)
                Base lid: 1
                LMC: 0
                SM lid: 0
                Capability mask: 0x00050000
                Port GUID: 0x6a05cafffec1b901
                Link layer: Ethernet
```

## Run RDMA perftest

In one shell, start the server with device 0.

```bash
ib_write_bw -d irdma0
```

In another shell, start the client with device 1, specify the IP of device 0.
The test result will show the max write bandwidth with single queue pair.
You can specify 8 queue pairs by adding `-q 8` to both sides.

```bash
ib_write_bw -d irdma1 192.168.99.110
```
```text
---------------------------------------------------------------------------------------
                    RDMA_Write BW Test
 Dual-port       : OFF          Device         : irdma1
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : OFF
 TX depth        : 128
 CQ Moderation   : 1
 Mtu             : 1024[B]
 Link type       : Ethernet
 GID index       : 1
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0x01 QPN 0x0003 PSN 0xc30267 RKey 0x69540273 VAddr 0x007e4ef245e000
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:192:168:99:111
 remote address: LID 0x01 QPN 0x0003 PSN 0x2ce2c7 RKey 0xbfde4945 VAddr 0x0075bee2e6f000
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:192:168:99:110
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      5000             1103.39            1103.38            0.017654
---------------------------------------------------------------------------------------
```

## Build MTL RDMA library

You can build entire MTL project with [build.sh](../build.sh), or use meson to build RDMA library only.

```bash
./build.sh
# or
cd <mtl>/rdma
meson setup build
sudo meson install -C build
# check package installed
pkg-config --libs mtl_rdma
# -L/usr/local/lib/x86_64-linux-gnu -lmtl_rdma
```

## Run TX sample app

Prepare a file (`test.yuv`) of 1920 * 1080 UYVY frames to send. You can refer to [run.md](run.md).

```bash
./build/app/RdmaVideoTxSample 192.168.99.110 20000 test.yuv
```

## Run RX sample app

You need SDL library to display the received frame.

```bash
./build/app/RdmaVideoRxSample 192.168.99.111 192.168.99.110 20000
```
