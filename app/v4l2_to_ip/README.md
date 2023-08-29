# V4L2 to IP application

## Introduction
The application reads video frames from V4L2 compliant device. And
transmits the video over IP network as RTP stream.

The application successfully ran on Intel Video-Capture-Design solution.
Details as below:
## Hardware
UZEL UM-H3910
- CPU: Intel® Core™ i7-1165G7
- Memory: 8G
- Video Input: 1 x HDMI
- Display Ouput: 4 x HDMI
- Ethernet: Intel® Ethernet Controller I225-LM
## Software
- OS: Ubuntu 20.04.3 LTS
- Boot Options: iommu=pt intel_iommu=on hugepages=2048 rdt=!l3cat,!l2cat efi=runtime art=virtallow clocksource=tsc tsc=reliable no_ipi_broadcast=1 nosoftlockup idle=poll audit=0 nmi_watchdog=0 irqaffinity=0 noht isolcpus=1-3 rcu_nocbs=1-3 nohz_full=1-3 intel_pstate=disable intel.max_cstate=0 intel_idle.max_cstate=0 processor.max_cstate=0 processor_idle.max_cstate=0 vt.handoff=7
- Complete all steps in doc#646935.
## Run
```bash
sudo media-ctl -r
sudo media-ctl -v -V "\"lt6911uxc a\":0 [fmt:UYVY/1920x1080]"
sudo media-ctl -v -V "\"Intel IPU6 CSI-2 1\":0 [fmt:UYVY/1920x1080]"
sudo media-ctl -v -V "\"Intel IPU6 CSI2 BE SOC\":0 [fmt:UYVY/1920x1080]"
sudo media-ctl -v -l "\"lt6911uxc a\":0 -> \"Intel IPU6 CSI-2 1\":0[1]"
sudo media-ctl -v -l "\"Intel IPU6 CSI-2 1\":1 -> \"Intel IPU6 CSI2 BE SOC\":0[5]"
sudo media-ctl -v -l "\"Intel IPU6 CSI2 BE SOC\":16 -> \"Intel IPU6 BE SOC capture 0\":0[5]"
sudo ./build/app/V4l2toIPApp /dev/video51 --log-status --ptp --tsn
```
## Expected Output
```bash
MT: * *    M T    D E V   S T A T E   * *
MT: DEV(0): Avr rate, tx: 1743.150045 Mb/s, rx: 0.017576 Mb/s, pkts, tx: 1646228, rx: 245
MT: CNI(0): eth_rx_rate 0 Mb/s, eth_rx_cnt 245
MT: PTP(0): time 1692904264838671714, 2023-08-24 19:10:27
MT: PTP(0): system clock offset max 259, synchronized
MT: PTP(0): delta avg 3, min -12, max 11, cnt 138
MT: PTP(0): correct_delta avg 25744798125, min -169289497586, max 60530855505, cnt 80
MT: PTP(0): path_delay avg 1413, min 1410, max 1416, cnt 80
MT: PTP(0): mode l4, sync cnt 80, expect avg 1:0@0.124482s
MT: PTP(0): rx time error 0, tx time error 0, delta result error 58
MT: TX_VIDEO_SESSION(0,0:v4l2_st20_tx): fps 49.991044, frame 500 pkts 1646200:1646199 inflight 407057:411550
MT: TX_VIDEO_SESSION(0,0): throughput 1742.887329 Mb/s: 0.000000 Mb/s, cpu busy 0.000000
MT: * *    E N D    S T A T E   * *
