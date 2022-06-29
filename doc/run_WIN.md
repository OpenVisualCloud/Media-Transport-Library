@page run_windows Running in Windows
# Run Guide
The Kahawai library required Windows netuio driver Windows virt2phys driver and huge page(windows server 2022) to run
## 1. System setup

#### 1.1 Enable test sign in Windows.
bcdedit /set loadoptions DISABLE_INTEGRITY_CHECKS
bcdedit /set TESTSIGNING ON

#### 1.2 Add huge page rights in Windows.
Open Local Security Policy snap-in, either:
Control Panel / Computer Management / Local Security Policy;
or Win+R, type secpol, press Enter.
Open Local Policies / User Rights Assignment / Lock pages in memory.
Add desired users or groups to the list of grantees.
Privilege is applied upon next logon. In particular, if privilege has been granted to current user, a logoff is required before it is available.

## 2 Install virt2phys Driver

##### 2.1.1 Download dpdk-kmods pack from:
git://dpdk.org/dpdk-kmods
Compile the virt2phys and netuio project using visual studio 2019

##### 2.1.2 Then, execute command in cmd:
```
devcon.exe install virt2phys.inf root\virt2phys
```

##### 2.1.3 Make sure that the driver was installed 

##### 2.1.4 When there is a problem with driver installation are needed more steps:
	
Test sign the driver using a test certificate and then boot the Windows in ¡°Test mode¡±, or

Use the boot time option to ¡°Disable driver signature enforcement¡±

##### 2.1.5 Manually install virt2phys steps for Windows Server:

From Device Manager, Action menu, select ¡°Add legacy hardware¡±.	
	
It will launch the ¡°Add Hardware Wizard¡±. Click ¡°Next¡±

Select second option ¡°Install the hardware that I manually select from a list¡±

On the next screen, ¡°Kernel bypass¡± will be shown as a device class
	
Select it and click ¡°Next¡±.

Click "Have Disk".

Find location of your virt2phys.inf driver.

Select it and click ¡°Next¡±.

The previously installed drivers will now be installed for the ¡°Virtual to physical address translator¡± device

#### 2.1.6 Here we just go through next and finish buttons.

## 3. Steps for netuio driver 
#### 3.1 Use devcon install netuio driver
Get devcon.exe from Windows WDK package, copy the devcon.exe to your netuio driver folder
execute command:
```
devcon.exe update netuio.inf "PCI\VEN_8086&DEV_1592"
```
#### 3.2 Manually install netuio driver
* Go to Device Manager -> Network Adapters.
* Right Click on target e810 network adapter -> Select Update Driver.
* Select "Browse my computer for driver software".
* In the resultant window, select "Let me pick from a list of available drivers on my computer".
* Select "DPDK netUIO for Network Adapter" from the list of drivers.
* The NetUIO.sys driver is successfully installed.

## 4. NIC setup:

#### 4.1 Update NIC FW and driver to latest version.
Refer to https://www.intel.com/content/www/us/en/download/15084/intel-ethernet-adapter-complete-driver-pack.html
#### 4.2 Copy the ICE driver related DDP file ice-1.3.26.0.pkg to the same directory as rxtxapp.exe file, can download from Intel site:
https://downloadmirror.intel.com/681886/26_6.zip
#### 4.3 Create the temp folder in root directory c:\temp

## 5. Run the sample application:

#### 5.1 Prepare source files:
Pls note the input yuv source file for sample app is the yuv422YCBCR10be pixel group format, not ffmpeg yuv422p10be pixel format. Kahawai include a simple tools to convert the format.
###### 5.1.1 Convert yuv422p10be to yuv422YCBCR10be
Below is the command to convert yuv422p10be file to yuv422YCBCR10be pg format(ST2110-20 supported pg format for 422 10bit)
```
.\build\app\ConvApp --width 1920 --height 1080 --in_pix_fmt yuv422p10be --out_pix_fmt yuv422YCBCR10be -i signal_be.yuv -o test.yuv
```
Below is the command to convert yuv422YCBCR10be pg format(ST2110-20 supported pg format for 422 10bit) to yuv422p10be file
```
.\build\app\ConvApp --width 1920 --height 1080 --in_pix_fmt yuv422YCBCR10be --out_pix_fmt yuv422p10be  -i test.yuv -o signal_out.yuv
```

#### 5.2 PTP setup(optional):
Precision Time Protocol (PTP) provides global microsecond accuracy timing of all essences. Typical deployment include a PTP grandmaster within the network, and clients use tools(ex. ptp4l) to sync with the grandmaster. Kahawai library also include a built-in PTP implementation, sample app provide a option to enable it, see 3.6 for how to enable. The built-in PTP is disabled as default, Kahawai will use the system time source(clock_gettime) as PTP clock. If built-in PTP is enabled, Kahawai will select the internal NIC time as PTP source.

#### 5.3 Prepare the required dll files
libcrypto-1_1-x64.dll, libgcc_s_seh-1.dll,libgtest.dll,libstdc++-6.dll,libwinpthread-1.dll ice.pkg to the same dir as the app

#### 5.4 Run sample app with json config
Below is the command to run one video tx/rx session with json config, customize the config item in json as your setup.
.\build\app\RxTxApp --config_file config\test_tx_1port_1v.json
Also you can bind the app to the cpu socket 0 ( if your NIC is inserted into the pcie slot belongs to cpu socket 0 )as following:
To identify the socket if you do not know it, in the NIC card driver property page, check the bus number, if the number is great than
0x80, then socket 1, else socket 0
```
start /Node 0 /B .\build\app\RxTxApp --config_file config\test_tx_1port_1v.json
```
If it runs well, you will see below similar log output periodically.
```
ST: * *    S T    D E V   S T A T E   * *
ST: DEV(0): Avr rate, tx: 2638 Mb/s, rx: 0 Mb/s, pkts, tx: 2613182, rx: 80
ST: DEV(0): Status: imissed 0 ierrors 0 oerrors 0 rx_nombuf 0
ST: PTP(0), time 1636076300487864574, 2021-11-05 09:38:20
ST: PTP(0), mode l4, delta: avr 9477, min 8477, max 10568, cnt 10, avg 9477
ST: CNI(0): eth_rx_cnt 80
ST: TX_VIDEO_SESSION(0,0): fps 60.099933, pkts build 2593192 burst 2593192
ST: * *    E N D    S T A T E   * 

Then run a rx in another node/port.
```
.\build\app\RxTxApp --config_file config/test_rx_1port_1v.json
```
If it runs well, you will see below similar log output periodically.

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
For the supported parameters in the json, please refer to [JSON configuration guide](configuration_guide.md) for detail.

#### 5.5 Available parameters in sample app
```
--config_file <URL>                  : the json config file path
--ptp                                : Enable the built-in Kahawai PTP, default is disabled and system time is selected as PTP time source
--lcores <lcore list>                : the DPDK lcore list for this run, e.g. --lcores 28,29,30,31. If not assigned, lib will allocate lcore from system socket cores.
--test_time <seconds>                : the run duration, unit: seconds
--rx_separate_lcore                  : If enabled, RX video session will run on dedicated lcores, it means TX video and RX video is not running on the same core.
--dma_dev <DMA1,DMA2,DMA3...>        : DMA dev list to offload the packet memory copy for RX video frame session.
--ebu                                : debug option, enable timing check for video rx streams
--pcapng_dump <n>                    : debug option, dump n packets from rx video streams to pcapng files. 
--promiscuous                        : debug option, enable RX promiscuous( receive all data passing through it regardless of whether the destination address of the data) mode for NIC.
--cni_thread                         : debug option, use a dedicated thread for cni messages instead of tasklet
--sch_session_quota <count>          : debug option, max sessions count for one lcore, unit: 1080P 60FPS TX
--p_tx_dst_mac <mac>                 : debug option, destination MAC address for primary port, debug usage only
--r_tx_dst_mac <mac>                 : debug option, destination MAC address for redundant port, debug usage only
--log_level <level>                  : debug option, set log level. e.g. debug, info, warning, error
--nb_tx_desc <count>                 : debug option, number of transmit descriptors for each NIC TX queue, affect the memory usage and the performance.
--nb_rx_desc <count>                 : debug option, number of receive descriptors for each NIC RX queue, affect the memory usage and the performance.
```

## 6. Tests:
Kahawai include many automate test cases based on gtest, below is the example command to run, customize the argument as your setup.
```
.\build\tests\KahawaiTest --p_port 0000:31:00.0 --r_port 0000:31:00.1
```
Also you can bind the app to the cpu socket 0 ( if your NIC is inserted into the pcie slot belongs to cpu socket 0 )as following:
```
start /Node 0 /B .\build\tests\KahawaiTest --p_port 0000:31:00.0 --r_port 0000:31:00.1
```

## 7. FAQs:
Below error generally means mbuf pool create fail as no enough huge pages available, try to allocate more.
```
ST: st_init, mbuf_pool create fail
```

#### 7.1 Link not connected
Below error indicate the link of physical port is not connected to a network, pls confirm the cable link is working.
```
ST: dev_create_port(0), link not connected
```

