# E800 Series Driver Guide

NIC setup steps for Intel® E810 and E830 Series Ethernet Adapters

## 1. Update Driver Version with Media Transport Library Patches

The Media Transport Library relies on certain rate limit patches that are currently not included in the E810/E830 driver. These patches need to be applied to the driver before rebuilding it.

> **Note:** Occasionally, after a system reboot, the operating system (Ubuntu) may automatically upgrade to a new kernel version. In such cases, it is important to remember to rebuild the driver to ensure compatibility with the new kernel version.

### 1.1. Download Driver Version 2.2.8

You can download the CVL 2.2.8 driver source code directly with the command:

```bash
wget https://downloadmirror.intel.com/859252/ice-2.2.8.tar.gz
```

(As an alternative, you can also visit <https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-e810-series-devices-under-linux.html> and select `Download ice-2.2.8.tar.gz`.

Make sure to select the 2.2.8 version from the available version options, as there may be newer versions available. It is important to note that version 2.2.8 is the latest version we have verified. Typically, we revisit driver version upgrades on a quarterly schedule.)

The steps are based on downloading the file `ice-2.2.8.tar.gz`.

### 1.2. Unzip 2.2.8 Driver and Enter into the Source Code Directory

```bash
tar xvzf ice-2.2.8.tar.gz
cd ice-2.2.8
```

### 1.3. Patch 2.2.8 Driver with Rate Limit Patches

Apply all the patches placed in [directory for ice_driver](../patches/ice_drv/2.2.8/).

```bash
git init
git add .
git commit -m "init version 2.2.8"
git am $mtl_source_code/patches/ice_drv/2.2.8/*.patch
```

Note: The variable `$mtl_source_code` should be set to the top directory of the Media Transport Library source code. Please ensure that it is correctly configured. Additionally, when running the `git am` command, please verify that it executes without any errors.

Use `git log` to check if the latest commit is `version: update to Kahawai_2.2.8`.

### 1.4. Build and Install the Driver

Please refer to the [below command for build and install](chunks/_build_install_ice_driver.md).

```{include} chunks/_build_install_ice_driver.md
```

#### 1.4.1. Linux Kernel Header

If you see the below error while running `make`, the cause is missing the Linux kernel header files.

```text
*** Kernel header files not in any of the expected locations.
```

Try to install them using below command:

```bash
# for Ubuntu
sudo apt-get install linux-headers-$(uname -r)
# for CentOS or RHEL
sudo yum install kernel-devel
```

#### 1.4.2. rmmod irdma

If you see the below error while running `rmmod ice`, try to run `sudo rmmod irdma` and repeat the above command again.

```text
rmmod: ERROR: Module ice is in use by: irdma
```

### 1.5. Verify Both the Driver and DDP Version

Please double-check the driver version by running the `dmesg` command. This will provide you with the necessary information to confirm the correct driver version.

```bash
sudo dmesg | grep "Intel(R) Ethernet Connection E800 Series Linux Driver"
```

```text
ice: Intel(R) Ethernet Connection E800 Series Linux Driver - version Kahawai_2.2.8
```

Similar steps to confirm the DDP version.

```bash
sudo dmesg | grep "The DDP package was successfully loaded"
```

```text
The DDP package was successfully loaded: ICE OS Default Package (mc) version 1.3.35.0
```

If the version is less than 1.3.35.0, please update it using the following commands. The DDP package can be found at `ddp/ice-1.3.35.0.pkg` within the top directory of the driver source code.

```bash
cd /usr/lib/firmware/updates/intel/ice/ddp
sudo cp <latest_ddp_dir>/ice-1.3.35.0.pkg ./
sudo rm ice.pkg
sudo ln -s ice-1.3.35.0.pkg ice.pkg
sudo rmmod ice
sudo modprobe ice
```

## 2. Update Firmware Version to Latest

This step is a one-time setup and can be skipped if you have already completed it for one Ethernet card.

### 2.1. Get the Latest Intel-Ethernet-Adapter-CompleteDriver-Pack

Download from <https://downloadcenter.intel.com/download/22283/Intel-Ethernet-Adapter-CompleteDriver-Pack>

### 2.2. Unzip NVMUpdatePackage

Note: Change the below version number if there's a new Intel-Ethernet-Adapter-CompleteDriver-Pack release. The steps are based on the 31.0 version.


```bash
unzip Release_31.0.zip
# For E810:
cd NVMUpdatePackage/E810
tar xvf E810_NVMUpdatePackage_v4_40_Linux.tar.gz
cd E810/Linux_x64/
# For E830:
cd NVMUpdatePackage/E830
tar xvf E830_NVMUpdatePackage_v<version>_Linux.tar.gz
cd E830/Linux_x64/
```

### 2.3. Run nvmupdate64e

Please follow the instructions provided in the nvmupdate64e tools guide. If an update is available, proceed with running the upgrade process as outlined in the nvmupdate64e tools.

```bash
sudo ./nvmupdate64e
```

### 2.4. Verify Firmware Version

To verify the firmware version, you can use the ethtool command with the interface name of your E810 or E830 card. Please replace "enp175s0f0" with the actual interface name in your setup.

```bash
ethtool -i enp175s0f0
```

If ethtool is not found in your system, please install it by `sudo apt-get install ethtool` or `sudo yum install ethtool`.

A correct setup should have an output similar to the following:

```text
driver: ice
version: Kahawai_2.2.8
# For E810:
firmware-version: 4.91 0x800214af 1.3909.0
# For E830:
firmware-version: 1.20 0x80017ef4 1.3909.0 
expansion-rom-version:
bus-info: 0000:af:00.0
supports-statistics: yes
supports-test: yes
supports-eeprom-access: yes
supports-register-dump: yes
supports-priv-flags: yes
```

## Next Steps
Proceed to [the MTL Run Guide](run.md#3-dpdk-pmd-setup) for further instructions.
