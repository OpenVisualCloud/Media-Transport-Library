# DMA setup for video RX offload

## 1. Overview:
Kahawai support DMA feature to offload the cpu memory copy for better rx video session density. DMA dev API is supported from DPDK 21.11, Kahawai introduce DMA feature from v0.7.2

## 2. DMA driver bind to PMD(vfio-pci) mode:

#### 2.1 Locate the available DMA port:
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

#### 2.2 Bind ports to PMD(vfio-pci):
Below example bind 0000:80:04.0,0000:80:04.1,0000:80:04.2 to PMD(vfio-pci) mode.
```bash
dpdk-devbind.py -b vfio-pci 0000:80:04.0
dpdk-devbind.py -b vfio-pci 0000:80:04.1
dpdk-devbind.py -b vfio-pci 0000:80:04.2
```

## 3. Pass the DMA port to RxTxApp:
Args --dma_dev was used the pass the DMA setup, below example bind 3 dma ports to the application
```bash
--dma_dev 0000:80:04.0,0000:80:04.1,0000:80:04.2
```
Logs will show the DMA usage info like below:
```bash
ST: RX_VIDEO_SESSION(1,0): pkts 2589325 by dma copy, dma busy 0.000000
ST: DMA(0), s 2589313 c 2589313 e 0 avg q 1
```
BTW, the gtest support --dma_dev also, pls pass the DMA setup for the DMA test.