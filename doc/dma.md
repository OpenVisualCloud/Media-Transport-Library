# DMA setup for video RX offload

## 1. Overview

The Intel® Media Transport Library supports a DMA feature to offload CPU memory copy for better RX video session density. The DMA device API is supported from DPDK version 21.11, and the DMA feature was introduced in version 0.7.2

## 2. DMA driver bind to PMD(vfio-pci) mode

### 2.1 Locate the available DMA port

```bash
dpdk-devbind.py -s | grep CBDMA
```

For DSA in SPR, pls search by idxd

```bash
dpdk-devbind.py -s | grep idxd
```

Pls check the output to find the VF BDF info, ex 0000:80:04.0 on the socket 1, 0000:00:04.0 on the socket 0, in below example.

```bash
0000:00:04.0 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.1 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.2 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.3 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.4 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.5 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.6 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:00:04.7 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.0 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.1 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.2 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.3 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.4 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.5 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.6 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
0000:80:04.7 'Sky Lake-E CBDMA Registers 2021' drv=ioatdma unused=vfio-pci
```

### 2.2 Bind ports to PMD(vfio-pci)

Below example bind 0000:80:04.0,0000:80:04.1,0000:80:04.2 to PMD(vfio-pci) mode.

```bash
dpdk-devbind.py -b vfio-pci 0000:80:04.0
dpdk-devbind.py -b vfio-pci 0000:80:04.1
dpdk-devbind.py -b vfio-pci 0000:80:04.2
```

## 3. Pass the DMA port to RxTxApp

The argument --dma_dev is used to pass the DMA setup. In the following example, three DMA ports are bound to the application:

```bash
--dma_dev 0000:80:04.0,0000:80:04.1,0000:80:04.2
```

The logs will display the DMA usage information as shown below:

```bash
ST: RX_VIDEO_SESSION(1,0): pkts 2589325 by dma copy, dma busy 0.000000
ST: DMA(0), s 2589313 c 2589313 e 0 avg q 1
```

By the way, gtest also supports the use of --dma_dev. Please pass the DMA setup for DMA testing as well.

## 3. DMA sample code for application usage

Refer to [dma_sample.c](../app/sample/dma/dma_sample.c) to learn how to use DMA on the application side. Use st_hp_virt2iova (for st_hp_malloc) or st_dma_map (for malloc) to obtain the IOVA address.
