# Run Guide on Windows

The Kahawai library required Windows netuio driver Windows virt2phys driver and huge page(windows server 2022) to run

## 1. System setup

### 1.1 Enable test sign in Windows

bcdedit /set loadoptions DISABLE_INTEGRITY_CHECKS
bcdedit /set TESTSIGNING ON

### 1.2 Add huge page rights in Windows

Open Local Security Policy snap-in, either:
Control Panel / Computer Management / Local Security Policy;
or Win+R, type secpol, press Enter.
Open Local Policies / User Rights Assignment / Lock pages in memory.
Add desired users or groups to the list of grantees.
Privilege is applied upon next logon. In particular, if privilege has been granted to current user, a logoff is required before it is available.

## 2 Install virt2phys Driver

### 2.1.1 Download dpdk-kmods pack from

git://dpdk.org/dpdk-kmods
Compile the virt2phys and netuio project using visual studio 2019

### 2.1.2 Then, execute command in cmd

```bash
devcon.exe install virt2phys.inf root\virt2phys
```

### 2.1.3 Make sure that the driver was installed

### 2.1.4 When there is a problem with driver installation are needed more steps

Test sign the driver using a test certificate and then boot the Windows in "Test mode", or

Use the boot time option to "Disable driver signature enforcement"

### 2.1.5 Manually install virt2phys steps for Windows Server

From Device Manager, Action menu, select "Add legacy hardware".

It will launch the "Add Hardware Wizard". Click "Next"

Select second option "Install the hardware that I manually select from a list"

On the next screen, "Kernel bypass" will be shown as a device class

Select it and click "Next".

Click "Have Disk".

Find location of your virt2phys.inf driver.

Select it and click "Next".

The previously installed drivers will now be installed for the "Virtual to physical address translator" device

### 2.1.6 Here we just go through next and finish buttons

## 3. Steps for netuio driver

### 3.1 Use devcon install netuio driver

Get devcon.exe from Windows WDK package, copy the devcon.exe to your netuio driver folder
execute command:

```bash
devcon.exe update netuio.inf "PCI\VEN_8086&DEV_1592"
```

### 3.2 Manually install netuio driver

* Go to Device Manager -> Network Adapters.
* Right Click on target e810 network adapter -> Select Update Driver.
* Select "Browse my computer for driver software".
* In the resultant window, select "Let me pick from a list of available drivers on my computer".
* Select "DPDK netUIO for Network Adapter" from the list of drivers.
* The NetUIO.sys driver is successfully installed.

## 4. NIC setup

### 4.1 Update NIC FW and driver to latest version

Refer to <https://www.intel.com/content/www/us/en/download/15084/intel-ethernet-adapter-complete-driver-pack.html>

### 4.2 Update the ICE DDP package file: ice.pkg

Get the latest ddp file(ice-1.3.30.0.pkg) from <https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-e810-series-devices-under-linux.html>, unzip the driver and goto ddp directory.  
Windows ICE driver will try to search DDP with path "c:\dpdk\lib\ice.pkg" or ".\ice.pkg", please put the latest ddp file there and rename to ice.pkg, otherwise it will see below error if you run the MTL app.

```bash
ice_load_pkg(): failed to search file path
ice_dev_init(): Failed to load the DDP package, Use safe-mode-support=1 to enter Safe Mode
```

### 4.3 Create the temp folder in root directory c:\temp

## 5. Windows TAP support enable

### 5.1 Download openVPN driver by searching "OpenVPN-2.5.6-I601-amd64.msi" and download the installation file

### 5.2 Install windows TAP driver

In the Control Panel->Network and internet->Network Connections, find the "OpenVPN TAP-Windows6" device, set the adaptor IP address, such as 192.168.2.2

### 5.3 Rebuild and install MTL lib with "-Denable_tap=true"

```bash
meson tap_build --prefix=c:\libmtl -Ddpdk_root_dir=${DPDK_SRC_DIR} -Denable_tap=true
ninja -C tap_build install
```

### 5.4 Run rxtxapp.exe

Ping 192.168.2.2 from other machine in the same network such as 192.168.2.3, if have reply, the TAP works.

## 6. Run and test

You can bind the app to the cpu socket 0 ( if your NIC is inserted into the pcie slot belongs to cpu socket 0 )as following:
To identify the socket if you do not know it, in the NIC card driver property page, check the bus number, if the number is great than
0x80, then socket 1, else socket 0, for example

```bash
start /Node 0 /B .\build\app\RxTxApp --config_file config\test_tx_1port_1v.json
```

Pls refer to section 3, 4, 5 in [linux run guide](run.md) for how to run the sample application, windows share same codebase with linux, the app/lib behavior is same.
