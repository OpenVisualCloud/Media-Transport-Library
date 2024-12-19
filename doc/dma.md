# DMA setup for video RX offload

## 1. Overview

The Media Transport Library features DMA support to enhance RX video session density by offloading memory copy operations from the CPU, thereby reducing CPU usage.

## 2. DMA driver bind to PMD(vfio-pci) mode

> For how to install DMA device driver on Windows, please refer to [here](./run_WIN.md#46-install-driver-for-dma-devices).

### 2.1. Locate the available DMA port

```bash
dpdk-devbind.py -s | grep CBDMA
```

For DSA in SPR, please search by idxd

```bash
dpdk-devbind.py -s | grep idxd
```

Please review the output below to locate the Virtual Function's Bus/Device/Function (BDF) information, such as '0000:80:04.0' for socket 1 or '0000:00:04.0' for socket 0, as illustrated in the example.

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

### 2.2. Bind ports to PMD(vfio-pci)

The example below demonstrates binding the devices '0000:80:04.0', '0000:80:04.1', and '0000:80:04.2' to Poll Mode Driver (PMD) using the vfio-pci module.

```bash
dpdk-devbind.py -b vfio-pci 0000:80:04.0
dpdk-devbind.py -b vfio-pci 0000:80:04.1
dpdk-devbind.py -b vfio-pci 0000:80:04.2
```

## 3. Pass the DMA configuration to lib

### 3.1. DMA configuration in RxTxApp

When utilizing the built-in application, simply use the `--dma_dev` argument to specify the DMA setup configuration. The following example demonstrates how to pass three DMA ports to the application:

```bash
--dma_dev 0000:80:04.0,0000:80:04.1,0000:80:04.2
```

### 3.2. DMA configuration in API

If you're directly interfacing with the API, the initial step involves incorporating DMA information into the `struct mtl_init_params` before making the `mtl_init` call. Subsequently, the initialization routine will attempt to parse and initialize the DMA device, and if the DMA is prepared, it will be added to the DMA list.

```bash
  /**
   * Optional. Dma(CBDMA or DSA) device can be used in the MTL.
   * DMA can be used to offload the CPU for copy the payload for video rx sessions.
   * See more from ST20_RX_FLAG_DMA_OFFLOAD in st20_api.h.
   * PCIE BDF path like 0000:80:04.0.
   */
  char dma_dev_port[MTL_DMA_DEV_MAX][MTL_PORT_MAX_LEN];
  /** Optional. The element number in the dma_dev_port array, leave to zero if no DMA */
  uint8_t num_dma_dev_port;
```

To enable DMA offloading, set the `ST20_RX_FLAG_DMA_OFFLOAD` flag in the `st20_rx_create` function, or `ST20P_RX_FLAG_DMA_OFFLOAD` when operating in pipeline mode. During the creation of the RX session, the system will attempt to locate a DMA device.
However, be aware that this process may fail if a suitable DMA device is not available, due to various reasons. For detailed information in case of failure, please consult the logs.

### 3.3. DMA logs

The logs below indicate that the PCI driver for the DMA device has been loaded successfully.

```bash
EAL: Probe PCI driver: dmadev_ioat (8086:b00) device: 0000:80:04.0 (socket 1)
IOAT: ioat_dmadev_probe(): Init 0000:80:04.0 on NUMA node 1
```

Below log shows the `0000:80:04.0` is registered into MTL.

```bash
MT: mt_dma_init(0), dma dev id 0 name 0000:80:04.0 capa 0x500000041 numa 1 desc 32:4096
```

Below log shows the RX session is correctly attached to a DMA device.

```bash
MT: mt_dma_request_dev(0), dma created with max share 16 nb_desc 128
MT: rv_init_dma(0), succ, dma 0 lender id 0
```

Below logs display the DMA usage information.

```bash
ST: RX_VIDEO_SESSION(1,0): pkts 2589325 by dma copy, dma busy 0.000000
ST: DMA(0), s 2589313 c 2589313 e 0 avg q 1
```

### 3.4. DMA socket

In a multi-socket system, each socket possesses its own DMA device, similar to NICs. Cross-socket traffic incurs significant latency; therefore, during MTL RX sessions, the system will attempt to utilize a DMA only if it resides on the same socket as the NICs.

### 3.5. DMA per core

To maximize the utilization of DMA resources, the MTL architecture is designed to use the same DMA device for all sessions running within the same core. Sharing the DMA device is safe in this context because the sessions within a single core share CPU resources, eliminating the need for spin locks.

## 4. Public DMA API for application usage

Besides using the internal DMA capabilities for RX video offload, applications can also leverage DMA through the public API.
To learn how to utilize DMA in your application, refer to the sample code in [dma_sample.c](../app/sample/dma/dma_sample.c).

The major DMA APIs listed below:

```bash
mtl_udma_create
mtl_udma_free
mtl_udma_copy
mtl_udma_submit
mtl_udma_completed
```

You can use the function `mtl_hp_virt2iova` when memory is allocated via `mtl_hp_malloc`, or use `mtl_dma_map` if memory is allocated using the standard malloc to obtain the IOVA (Input/Output Virtual Address) necessary for DMA operations with hardware.
