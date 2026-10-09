# E800 Series Driver Guide

NIC setup steps for Intel® E810 and E830 Series Ethernet Adapters

## 1. Update Driver Version with Media Transport Library Patches

The Media Transport Library needs rate limit patches that the E810/E830 driver does not include. Apply these patches to the driver, then rebuild it.

> **Note:** After a reboot, Ubuntu can upgrade to a new kernel version. If the kernel changes, rebuild the driver for the new kernel.

### 1.1. Download the Driver Source Code

`versions.env` in the Media Transport Library source tree holds the driver version to use. Read the file to set `ICE_VER` and `ICE_DMID`, then download the source code.

Note: point `$mtl_source_code` to the top source code tree of Media Transport Library.

```bash
. $mtl_source_code/versions.env
wget https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz
```

As an alternative, visit <https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-e810-series-devices-under-linux.html> and select the archive for that version.

The download page can show newer driver versions. Select the version from `versions.env`, because that is the version we verified. We review driver version upgrades every quarter.

The next steps use the file `ice-${ICE_VER}.tar.gz`.

### 1.2. Unzip the Driver and Enter the Source Code Directory

```bash
tar xvzf ice-${ICE_VER}.tar.gz
cd ice-${ICE_VER}
```

### 1.3. Patch the Driver with Rate Limit Patches

The [ice_drv patch folder](../patches/ice_drv/) holds one folder per driver version. `ICE_VER` selects the correct one.

```bash
git init
git add .
git commit -m "init version ${ICE_VER}"
git am $mtl_source_code/patches/ice_drv/${ICE_VER}/*.patch
```

Use `git log` to check that the latest commit updates the driver version. Every patch folder ends with this commit. For driver version 2.6.6 its subject is `version: update to Kahawai_2.6.6`, but some older driver versions use a different format.

### 1.4. Build and Install the Driver

Run the [build and install commands](chunks/_build_install_ice_driver.md) that follow.

```{include} chunks/_build_install_ice_driver.md
```

#### 1.4.1. Linux Kernel Header

If `make` shows the following error, the Linux kernel header files are missing.

```text
*** Kernel header files not in any of the expected locations.
```

Install them with the following command:

```bash
# for Ubuntu
sudo apt-get install linux-headers-$(uname -r)
# for CentOS or RHEL
sudo yum install kernel-devel
```

#### 1.4.2. rmmod irdma

If `rmmod ice` shows the following error, run `sudo rmmod irdma`, then repeat the command.

```text
rmmod: ERROR: Module ice is in use by: irdma
```

### 1.5. Verify Both the Driver and DDP Version

Verify the driver version with the `dmesg` command.
Every check below reads only the newest matching line.
`dmesg` keeps every earlier line, and the boot-time line still reports the version from before the install.

```bash
sudo dmesg | grep "Intel(R) Ethernet Connection E800 Series Linux Driver" | tail -1
```

```text
ice: Intel(R) Ethernet Connection E800 Series Linux Driver - version Kahawai_<ICE_VER>
```

Use similar steps to verify the DDP version.

```bash
sudo dmesg | grep "The DDP package was successfully loaded" | tail -1
```

```text
The DDP package was successfully loaded: ICE OS Default Package (mc) version <installed_version>
```

The `dmesg` line carries no BDF, so it cannot say which of several E800-series PFs loaded the package.
`devlink` reports the DDP version as `fw.app` for the PF BDF you name.
It still reports that version after you bind the PF's VFs to `vfio-pci`.
`devlink dev info` needs no root, so it works when `kernel.dmesg_restrict` blocks `dmesg`.
Give `devlink dev info` the BDF that `ethtool -i` reports as `bus-info`. Replace the BDF below with the one in your setup.

```bash
devlink dev info pci/0000:af:00.0 | grep fw.app
```

```text
        fw.app.name ICE OS Default Package
        fw.app 1.3.59.0
        fw.app.bundle_id 0xc0000001
```

`ethtool -i` itself does not report the DDP version.
The third field of its `firmware-version` is the UNDI version, which `devlink` reports as `fw.undi`.

The Media Transport Library needs DDP version 1.3.35.0 or higher.
The `sudo make install` command in Section 1.4 installs the DDP package from the driver source tree together with the module.
The manual steps below apply when the driver came from another place, such as a distribution package or the in-tree module.
They also read the package from the driver source tree.
If you do not have that tree, use Sections 1.1 and 1.2 first to get it.

If the version is lower than 1.3.35.0, install the DDP package from the driver source tree's `ddp` folder.
Each driver version can hold a different DDP version, so read the filename from the folder.
Run the following command from the top of the driver source tree.

```bash
ls ddp/ice-*.pkg
```

```text
ddp/ice-1.3.59.0.pkg
```

Still from that tree, the commands below read the DDP filename from the `ddp` folder themselves, so you do not type a version.

Prepare the host first, because `rmmod ice` fails while any device still uses the module:

- Disable the VFs on every E800-series PF in the host that the `ice` module serves, one command per PF. For a two-port card, run `sudo $mtl_source_code/script/nicctl.sh disable_vf 0000:af:00.0` and then the same command for `0000:af:00.1`. Replace the BDFs with the ones in your setup.
- Run `sudo rmmod irdma`, because `ice` cannot unload while `irdma` uses it. Ignore the message when `irdma` is not loaded.

```bash
DDP_PKG=$(ls ddp/ice-*.pkg) && \
sudo mkdir -p /lib/firmware/updates/intel/ice/ddp && \
sudo cp "${DDP_PKG}" /lib/firmware/updates/intel/ice/ddp/ && \
sudo ln -sf "$(basename "${DDP_PKG}")" /lib/firmware/updates/intel/ice/ddp/ice.pkg && \
sudo rmmod ice
sudo modprobe ice
```

Repeat the DDP check: `sudo dmesg | grep "The DDP package was successfully loaded" | tail -1`.
The load worked when the newest line reports the version of the `.pkg` file you copied.

The load failed if that line still reports the old version, or if no such line appeared.
Read `sudo dmesg | tail` for an `ice` message.
Also look in `/lib/firmware/updates/intel/ice/ddp` for a device-specific `ice-<serial>.pkg`, which overrides the `ice.pkg` symlink.
The copy and the symlink are already in place, and the boot image is still untouched.
Remove whatever holds the module, then run the block above again.

Section 1.4's `sudo make install` also refreshes the boot image, and the manual steps do not.
At early boot, `ice` can load the DDP package from the boot image.
Without the refresh, the driver can load the old package again after the next reboot.

Refresh it now that the new package is loaded.
Use `update-initramfs` on Ubuntu or `dracut` on CentOS or RHEL, never both.
Name the kernel, because `update-initramfs -u` with no `-k` refreshes the image of the highest installed kernel.
After a kernel upgrade, that kernel is not the running one.

```bash
# for Ubuntu
sudo update-initramfs -u -k "$(uname -r)"
# for CentOS or RHEL
sudo dracut --force --kver "$(uname -r)"
```

To refresh the image of a kernel other than the running one, run the command again.
Give that kernel's version in place of `$(uname -r)`.

The reload does not restore the VFs, so create them again on every PF you disabled, one command per PF.
For a two-port card, run `sudo $mtl_source_code/script/nicctl.sh create_vf 0000:af:00.0` and then the same command for `0000:af:00.1`.
[The MTL Run Guide](run.md#3-dpdk-pmd-setup) shows this step.
`create_vf` makes 6 VFs, so give the count as a last argument if you had a different number.
The Media Transport Library does not use `irdma`, so leave it unloaded.
If something else on this host needs RDMA, run `sudo modprobe irdma`.

## 2. Update Firmware Version to Latest

This is a one-time setup. If you already did it for one Ethernet card, skip this step.

### 2.1. Get the Latest Intel-Ethernet-Adapter-CompleteDriver-Pack

Download from <https://downloadcenter.intel.com/download/22283/Intel-Ethernet-Adapter-CompleteDriver-Pack>

### 2.2. Unzip NVMUpdatePackage

Note: If a new Intel-Ethernet-Adapter-CompleteDriver-Pack release exists, change the version number in the following commands. The steps use the 31.0 version.

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

Follow the instructions in the nvmupdate64e tools guide. If an update is available, run the upgrade.

```bash
sudo ./nvmupdate64e
```

### 2.4. Verify Firmware Version

To verify the firmware version, run the `ethtool` command with the interface name of your E810 or E830 card. Replace `enp175s0f0` with the interface name in your setup.

```bash
ethtool -i enp175s0f0
```

If your system does not have `ethtool`, install it with `sudo apt-get install ethtool` or `sudo yum install ethtool`.

A correct setup shows output similar to the following:

```text
driver: ice
version: Kahawai_<ICE_VER>
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

Go to [the MTL Run Guide](run.md#3-dpdk-pmd-setup) for the next instructions.
