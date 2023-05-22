# Run Guide

The Intel® Media Transport Library requires VFIO (IOMMU) and huge pages to run, but it also supports non-root run, making it easy to deploy within Docker/Kubernetes environments.

## 1. System setup

### 1.1 Enable IOMMU(VT-D and VT-X) in BIOS

### 1.2 Enable IOMMU in kernel

#### 1.2.1 Ubuntu/Debian

Edit GRUB_CMDLINE_LINUX_DEFAULT item in /etc/default/grub file, append below parameters into GRUB_CMDLINE_LINUX_DEFAULT item.

```bash
intel_iommu=on iommu=pt
```

then:

```bash
sudo update-grub
sudo reboot
```

#### 1.2.2 Centos

```bash
sudo grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"
sudo reboot
```

### 1.3 Double check iommu_groups is created by kernel after reboot

```bash
ls -l /sys/kernel/iommu_groups/
```

## 2. NIC setup

### 2.1 Update NIC FW and driver to latest version

Refer to <https://www.intel.com/content/www/us/en/download/15084/intel-ethernet-adapter-complete-driver-pack.html>

After upgrading, please double-check that the DDP version is correct (i.e., greater than 1.3.30.0) by running the dmesg command.

```bash
The DDP package was successfully loaded: ICE OS Default Package (mc) version 1.3.30.0
```

If the DDP version is not the latest, please use the following command to update it. The DDP package can be found in the latest Intel ice driver.

```bash
cd /usr/lib/firmware/updates/intel/ice/ddp
cp <latest_ddp_dir>/ice-1.3.30.0.pkg ./
rm ice.pkg
ln -s ice-1.3.30.0.pkg ice.pkg
rmmod ice
modprobe ice
```

### 2.2 Bind NIC to DPDK PMD mode

Below is the command to bind the BDF port 0000:af:00.0 to PF PMD mode. Please customize the BDF port based on your setup.

```bash
sudo ./script/nicctl.sh bind_pmd 0000:af:00.0
```

If you see the following error, please double-check if IOMMU is enabled:

```bash
Error: bind failed for 0000:af:00.0 - Cannot bind to driver vfio-pci: [Errno 19] No such device
Error: unbind failed for 0000:af:00.0 - Cannot open /sys/bus/pci/drivers//unbind: [Errno 13] Permission denied:
```

```bash
ls -l /sys/kernel/iommu_groups/
```

## 3. Run the sample application

### 3.1 VFIO access for non-root run

To add VFIO device permissions to the current user:

```bash
# change <USER> to the user name currently login.
sudo chown -R <USER>:<USER> /dev/vfio/
```

### 3.2 Huge page setup

e.g Enable 2048 2M huge pages, in total 4g memory.

```bash
sudo sysctl -w vm.nr_hugepages=2048
```

### 3.3 Prepare source files

Please note that the input YUV source file for the sample application is in the RFC4175 YUV422BE10 (big-endian 10-bit) pixel group format, which is defined in the ST2110 specification. This project includes a simple tool to convert the format from YUV422 planar 10-bit little-endian format.

#### 3.3.1 Prepare a yuv422p10le file

The following command shows how to decode two frames from the encoder file and convert it from 420 to 422 planar file. Change the 'vframes' value if you want to generate more frames.

```bash
wget https://www.larmoire.info/jellyfish/media/jellyfish-3-mbps-hd-hevc-10bit.mkv
ffmpeg -i jellyfish-3-mbps-hd-hevc-10bit.mkv -vframes 2 -c:v rawvideo yuv420p10le_1080p.yuv
ffmpeg -s 1920x1080 -pix_fmt yuv420p10le -i yuv420p10le_1080p.yuv -pix_fmt yuv422p10le yuv422p10le_1080p.yuv
```

#### 3.3.2 Convert yuv422p10le to yuv422rfc4175be10

Below is the command to convert yuv422p10le file to yuv422rfc4175be10 pg format(ST2110-20 supported pg format for 422 10bit)

```bash
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -out_pix_fmt yuv422rfc4175be10 -o yuv422rfc4175be10_1080p.yuv
```

The yuv422rfc4175be10 files can be viewed by YUV Viewer tools(<https://github.com/IENT/YUView>), below is the custom layout.
<div align="center">
<img src="png/yuview_yuv422rfc4175be10_layout.png" align="center" alt="yuview yuv422rfc4175be10 custom layout">
</div>

#### 3.3.3 Convert yuv422rfc4175be10 back to yuv422p10le

Below is the command to convert yuv422rfc4175be10 pg format(ST2110-20 supported pg format for 422 10bit) to yuv422p10le file

```bash
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv422rfc4175be10 -i yuv422rfc4175be10_1080p.yuv -out_pix_fmt yuv422p10le -o out_yuv422p10le_1080p.yuv
```

#### 3.3.4 v210 support

This tools also support v210 format, use "v210" for the in_pix_fmt/out_pix_fmt args instead.

```bash
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv422rfc4175be10 -i yuv422rfc4175be10_1080p.yuv -out_pix_fmt v210 -o v210_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt v210 -i v210_1080p.yuv -out_pix_fmt yuv422rfc4175be10 -o out_yuv422rfc4175be10_1080p.yuv
```

#### 3.3.5 yuv422 12bit support

```bash
ffmpeg -s 1920x1080 -pix_fmt yuv420p10le -i yuv420p10le_1080p.yuv -pix_fmt yuv422p12le yuv422p12le_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv422p12le -i yuv422p12le_1080p.yuv -out_pix_fmt yuv422rfc4175be12 -o yuv422rfc4175be12_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv422rfc4175be12 -i yuv422rfc4175be12_1080p.yuv -out_pix_fmt yuv422p12le -o out_yuv422p12le_1080p.yuv
```

#### 3.3.6 yuv444 10bit support

```bash
ffmpeg -s 1920x1080 -pix_fmt yuv420p10le -i yuv420p10le_1080p.yuv -pix_fmt yuv444p10le yuv444p10le_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv444p10le -i yuv444p10le_1080p.yuv -out_pix_fmt yuv444rfc4175be10 -o yuv444rfc4175be10_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv444rfc4175be10 -i yuv444rfc4175be10_1080p.yuv -out_pix_fmt yuv444p10le -o out_yuv444p10le_1080p.yuv
```

#### 3.3.7 yuv444 12bit support

```bash
ffmpeg -s 1920x1080 -pix_fmt yuv420p10le -i yuv420p10le_1080p.yuv -pix_fmt yuv444p12le yuv444p12le_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv444p12le -i yuv444p12le_1080p.yuv -out_pix_fmt yuv444rfc4175be12 -o yuv444rfc4175be12_1080p.yuv
./build/app/ConvApp -width 1920 -height 1080 -in_pix_fmt yuv444rfc4175be12 -i yuv444rfc4175be12_1080p.yuv -out_pix_fmt yuv444p12le -o out_yuv444p12le_1080p.yuv
```

### 3.4 PTP setup(optional)

The Precision Time Protocol (PTP) enables global microsecond accuracy timing of all essences and is typically deployed with a PTP grandmaster within the network, while clients use tools such as ptp4l to synchronize with it. This library also includes a built-in PTP implementation, and a sample application provides an option to enable it. See section 3.6 for instructions on how to enable it.

By default, the built-in PTP is disabled, and the user application's system time source (clock_gettime) is used as the PTP clock. However, if the built-in PTP is enabled, the internal NIC time will be selected as the PTP source.

#### 3.4.1 ptp4l setup sample

Firstly run ptp4l to sync the PHC time with grandmaster, customize the interface as your setup.

```shell
sudo ptp4l -i ens801f2 -m -s -H
```

Then run phc2sys to sync the PHC time to system time, please make sure NTP service is disabled as it has conflict with phc2sys.

```shell
sudo phc2sys -s ens801f2 -m -w
```

### 3.5 Run sample app with json config

Below is the command to run one video tx/rx session with json config, customize the config item in json as your setup.

```bash
./build/app/RxTxApp --config_file config/test_tx_1port_1v.json
```

If it runs well, you will see below similar log output periodically.

```bash
ST: * *    S T    D E V   S T A T E   * *
ST: DEV(0): Avr rate, tx: 2638 Mb/s, rx: 0 Mb/s, pkts, tx: 2613182, rx: 80
ST: DEV(0): Status: imissed 0 ierrors 0 oerrors 0 rx_nombuf 0
ST: PTP(0), time 1636076300487864574, 2021-11-05 09:38:20
ST: PTP(0), mode l4, delta: avr 9477, min 8477, max 10568, cnt 10, avg 9477
ST: CNI(0): eth_rx_cnt 80
ST: TX_VIDEO_SESSION(0,0): fps 60.099933, pkts build 2593192 burst 2593192
ST: * *    E N D    S T A T E   * 
```

Then run a rx in another node/port.

```bash
./build/app/RxTxApp --config_file config/test_rx_1port_1v.json
```

If it runs well, you will see below similar log output periodically.

```bash
ST: * *    S T    D E V   S T A T E   * *
ST: DEV(0): Avr rate, tx: 0 Mb/s, rx: 2614 Mb/s, pkts, tx: 12, rx: 2589728
ST: DEV(0): Status: imissed 0 ierrors 0 oerrors 0 rx_nombuf 0
ST: PTP(0), time 1636075100571923971, 2021-11-05 09:18:20
ST: PTP(0), mode l2, delta: avr 7154, min -5806, max 10438, cnt 4, avg 6198
ST: CNI(0): eth_rx_cnt 52
ST: RX_VIDEO_SESSION(0,0): fps 59.899925, received frames 599, pkts 2589686
app_rx_video_stat(0), fps 59.899932, 599 frame received
ST: * *    E N D    S T A T E   * *
```

This project also provide many loop test(1 port as tx, 1 port as rx) config file , pls refer to [loop config](../tests/script/).

For the supported parameters in the json, please refer to [JSON configuration guide](configuration_guide.md) for detail.

### 3.6 Available parameters in sample app

```bash
--config_file <URL>                  : the json config file path
--ptp                                : Enable the built-in PTP, default is disabled and system time is selected as PTP time source
--lcores <lcore list>                : the DPDK lcore list for this run, e.g. --lcores 28,29,30,31. If not assigned, lib will allocate lcore from system socket cores.
--test_time <seconds>                : the run duration, unit: seconds
--rx_separate_lcore                  : If enabled, RX video session will run on dedicated lcores, it means TX video and RX video is not running on the same core.
--dma_dev <DMA1,DMA2,DMA3...>        : DMA dev list to offload the packet memory copy for RX video frame session.
--runtime_session                    : start instance before create video/audio/anc sessions, similar to runtime tx/rx create.

--ebu                                : debug option, enable timing check for video rx streams.
--pcapng_dump <n>                    : debug option, dump n packets from rx video streams to pcapng files.
--rx_video_file_frames <n>           : debug option, dump the received video frames to a yuv file, n is dump file size in frame unit.
--rx_video_fb_cnt<n>                 : debug option, the frame buffer count.
--promiscuous                        : debug option, enable RX promiscuous( receive all data passing through it regardless of whether the destination address of the data) mode for NIC.
--cni_thread                         : debug option, use a dedicated thread for cni messages instead of tasklet.
--sch_session_quota <count>          : debug option, max sessions count for one lcore, unit: 1080P 60FPS TX.
--p_tx_dst_mac <mac>                 : debug option, destination MAC address for primary port.
--r_tx_dst_mac <mac>                 : debug option, destination MAC address for redundant port.
--log_level <level>                  : debug option, set log level. e.g. debug, info, notice, warning, error.
--nb_tx_desc <count>                 : debug option, number of transmit descriptors for each NIC TX queue, affect the memory usage and the performance.
--nb_rx_desc <count>                 : debug option, number of receive descriptors for each NIC RX queue, affect the memory usage and the performance.
--tasklet_time                       : debug option, enable stat info for tasklet running time.
--tsc                                : debug option, force to use tsc pacing.
--pacing_way <way>                   : debug option, set pacing way, available value: "auto", "rl", "tsc", "tsc_narrow", "ptp", "tsn".
--shaping <shaping>                  : debug option, set st21 shaping type, available value: "narrow", "wide".
--vrx <n>                            : debug option, set st21 vrx value, refer to st21 spec for possible vrx value.
--mono_pool                          : debug option, use mono pool for all tx and rx queues(sessions).
--tasklet_thread                     : debug option, run the tasklet under thread instead of a pinned lcore.
--tasklet_sleep                      : debug option, enable sleep if all tasklet report done status.
--tasklet_sleep_us                   : debug option, set the sleep us value if tasklet decide to enter sleep state.
--app_thread                         : debug option, run the app thread under a common os thread instead of a pinned lcore.
--rxtx_simd_512                      : debug option, enable dpdk simd 512 path for rx/tx burst function, see --force-max-simd-bitwidth=512 in dpdk for detail.
--rss_mode <mode>                    : debug option, available modes: "l3_l4_dst_port_only", "l3_da_l4_dst_port_only", "l4_dst_port_only", "none".
--tx_no_chain                        : debug option, use memcopy rather than mbuf chain for tx payload.
--multi_src_port                     : debug option, use multiple src port for st20 tx stream.
--audio_fifo_size <count>            : debug option, the audio fifo size between packet builder and pacing.
--dhcp                               : debug option, enable DHCP for all ports.
```

## 4. Tests

This project include many automate test cases based on gtest, below is the example command to run, customize the argument as your setup.

```bash
./build/tests/KahawaiTest --p_port 0000:af:00.0 --r_port 0000:af:00.1
```

BTW, the test required large huge page settings, pls expend it to 8g.

```bash
sudo sysctl -w vm.nr_hugepages=4096
```

## 5. FAQs

### 5.1 Notes after reboot

After a reboot, the operating system may update to a new kernel version. In such cases, remember to rebuild the firmware/DDP version.

### 5.2 Notes for non-root run

When running as non-root user, there may be some additional resource limits that are imposed by the system.

### 5.2.1 RLIMIT_MEMLOCK

RLIMIT_MEMLOCK (amount of pinned pages the process is allowed to have), if you see below error at start up, it's likely caused by too small RLIMIT_MEMLOCK settings.

```bash
EAL: Cannot set up DMA remapping, error 12 (Cannot allocate memory)
EAL: 0000:af:01.0 DMA remapping failed, error 12 (Cannot allocate memory)
EAL: Requested device 0000:af:01.0 cannot be used
```

Pls

```bash
# Edit /etc/security/limits.conf, append below two lines at the end of file, change <USER> to the user name currently login.
<USER>    hard   memlock           unlimited
<USER>    soft   memlock           unlimited
sudo reboot
```

After reboot, double check the limit is disabled.

```bash
ulimit -a | grep "max locked memory"
max locked memory       (kbytes, -l) unlimited
```

### 5.3 BDF port not bind to DPDK PMD mode

The following error indicates that the port driver is not configured to DPDK PMD mode. Please run nicctl.sh to configure it:

```bash
ST: st_dev_get_socket, failed to locate 0000:86:20.0. Please run nicctl.sh
```

### 5.4 Hugepage not available

If you encounter the following hugepage error while running, it is likely caused by the absence of 1G or 2M huge pages in the current setup.

```bash
EAL: FATAL: Cannot get hugepage information.
EAL: Cannot get hugepage information.
```

This error message usually indicates that the mbuf pool creation has failed due to insufficient huge pages. Please try to allocate more huge pages.

```bash
ST: st_init, mbuf_pool create fail
```

### 5.5 No access to vfio device

If you encounter the following error message, please grant the current user access to the dev:

```bash
EAL: Cannot open /dev/vfio/147: Permission denied
EAL: Failed to open VFIO group 147
```

### 5.6 Link not connected

The following error indicates that the physical port link is not connected to a network. Please confirm that the cable link is working properly.

```bash
ST: dev_create_port(0), link not connected
```

### 5.7 Bind BDF port back to kernel mode

```bash
sudo ./script/nicctl.sh bind_kernel 0000:af:00.0
```

### 5.8 How to find the BDF number

```bash
lspci | grep Eth
```

### 5.9 Lower fps if ptp4l&phc2sys is enabled

You may have noticed a similar epoch drop log, which is likely caused by both NTP and phc2sys adjusting the system. To address this issue, please disable the NTP service.

```bash
MT: DEV(0): Avr rate, tx: 4789 Mb/s, rx: 0 Mb/s, pkts, tx: 4525950, rx: 9
MT: PTP(0): time 1676254936223518377, 2023-02-13 10:22:16
MT: CNI(0): eth_rx_cnt 9
MT: TX_VIDEO_SESSION(0,0:app_tx_video_0): fps 27.499879, frame 275 pkts 4526532:4525984 inflight 279856:279869, cpu busy 75.286346
MT: TX_VIDEO_SESSION(0,0): dummy pkts 550, burst 550
MT: TX_VIDEO_SESSION(0,0): mismatch epoch troffset 275
MT: TX_VIDEO_SESSION(0,0): epoch drop 275
```
