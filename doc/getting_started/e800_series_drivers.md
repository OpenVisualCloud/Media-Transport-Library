# E800 Series Driver Guide

Driver setup for Intel® E810 and E830 Series Ethernet Adapters.

The Media Transport Library needs these parts on the host:

| Part | Version | Section |
| --- | --- | --- |
| Intel `ice` driver | 2.7.6, `ICE_VER` in `versions.env`: the version that we verified | [1](#1-check-the-driver), [2](#2-install-the-driver) |
| DDP package | 1.3.35.0 or later. The driver installs it. | [3](#3-check-the-ddp-package) |
| NVM firmware | The latest release | [4](#4-update-the-firmware) |

The `ice` module that comes with the Linux kernel (the in-tree driver) is not sufficient.
It gives a VF no rate limiter. The rate-limit pacing of the library then stops the application with a segmentation fault in `iavf_tm_node_add`.
Intel releases the driver as source code only, so you build it for your kernel.

This guide installs only the verified version. For a driver before 2.7.6, or a DDP package before 1.3.35.0, see [Earlier E800 Drivers and DDP Packages](e800_legacy.md).

## Before You Start

* **Your distribution selects the procedure of section 2.**

  | Distribution | Procedure |
  | --- | --- |
  | Ubuntu, Debian | [2.1](#21-ubuntu-and-debian): one script builds and installs the driver. On Debian, install the kernel headers with `apt-get` first. |
  | RHEL, CentOS Stream, Rocky Linux | [2.2](#22-rhel-centos-stream-and-rocky-linux): build an RPM package from the driver source, and install it. |
  | Other | [2.3](#23-other-distributions): build and install the driver by hand. |

* **The driver reload removes the VFs** of each E800-series PF. Stop each application that uses a VF of an E800-series PF before you install the driver. After the install, create the VFs again, as [the Run Guide](run.md#4-bind-the-nic-ports-to-dpdk) shows.
* **A new kernel needs a new build of the driver.** Intel gives no DKMS package, so the driver does not follow a kernel upgrade by itself. Do section 2 again after each kernel upgrade, before you use MTL.

## 1. Check the Driver

Replace `enp175s0f0` with the interface name of your E810 or E830 port.

```bash
ethtool -i enp175s0f0 | head -3
```

```text
driver: ice
version: 2.7.6
firmware-version: 4.91 0x800214af 1.3909.0
```

If the `version` line shows `2.7.6`, go to [section 3](#3-check-the-ddp-package). For each other value, do [section 2](#2-install-the-driver).
A kernel release, for example `6.8.0-87-generic`, is the in-tree driver.

## 2. Install the Driver

### 2.1. Ubuntu and Debian

Run the commands from the top of the Media Transport Library source tree.

```bash
./script/mtl_install_dependencies.sh ice
./script/mtl_build_drivers.sh --driver ice
```

`mtl_install_dependencies.sh ice` installs the build tools, the headers and the extra modules of the running kernel. It runs only on Ubuntu. On Debian, use this command in its place:

```bash
sudo apt-get install gcc make tar gzip wget "linux-headers-$(uname -r)"
```

The driver does not build for the Debian cloud kernel (`-cloud-amd64`), which has no hwmon support. Use the kernel of `linux-image-amd64`.

`mtl_build_drivers.sh` downloads the driver version in `versions.env`, builds it, installs it with its DDP package, and loads it.
It does nothing when that version is already installed for the running kernel. Use `--force` to build it again.

Check the driver that runs now:

```bash
ethtool -i enp175s0f0 | head -3
```

The `version` line must show `2.7.6`. If it does not, see [Driver Problems](troubleshooting.md#2-driver-problems).

### 2.2. RHEL, CentOS Stream and Rocky Linux

The driver source makes an RPM package for the running kernel. `$mtl_source_code` is the top of the Media Transport Library source tree.

1. Install the build tools and the headers of the running kernel:

   ```bash
   sudo dnf install gcc make tar gzip wget rpm-build kernel-rpm-macros kernel-abi-stablelists elfutils-libelf-devel "kernel-devel-$(uname -r)"
   ```

2. Download the driver version of `versions.env`, and build the RPM package:

   ```bash
   . $mtl_source_code/versions.env
   wget https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz
   rpmbuild -tb ice-${ICE_VER}.tar.gz
   ```

   The last lines of the `rpmbuild` output give the path of the package, usually `~/rpmbuild/RPMS/x86_64/ice-2.7.6-1.x86_64.rpm`.

3. Install the package, and load the new driver:

   ```bash
   sudo dnf localinstall ~/rpmbuild/RPMS/x86_64/ice-${ICE_VER}-1.x86_64.rpm
   sudo rmmod irdma 2>/dev/null
   sudo rmmod ice
   sudo modprobe ice
   ```

4. Check the driver that runs now:

   ```bash
   ethtool -i enp175s0f0 | head -3
   ```

   The `version` line must show `2.7.6`.

The package also installs the DDP package of the driver. Intel tests the RPM build on Red Hat distributions only.

### 2.3. Other Distributions

Install `gcc`, `make`, `tar`, `gzip`, `wget` and the headers of the running kernel. Then build and install the driver:

```bash
. $mtl_source_code/versions.env
wget https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz
tar xzf ice-${ICE_VER}.tar.gz
cd ice-${ICE_VER}/src
make
sudo make install
sudo rmmod irdma 2>/dev/null
sudo rmmod ice
sudo modprobe ice
ethtool -i enp175s0f0 | head -3   # the version line must show 2.7.6
```

`make install` installs the module and the DDP package of the source tree, and refreshes the boot image.
The source code is also on GitHub, as the tag `v<version>` of [intel/ethernet-linux-ice](https://github.com/intel/ethernet-linux-ice).

The Intel download page can show a newer driver version. Use the version in `versions.env`, because that is the version that we verified. We examine new driver versions each quarter.

## 3. Check the DDP Package

The Media Transport Library needs DDP version 1.3.35.0 or later. Section 2 installs the DDP package of the driver together with the module, so a new install has a sufficient version.

`devlink` shows the DDP version as `fw.app` for the PF BDF that you name. It needs no root.
Use the BDF that `ethtool -i` shows as `bus-info`. Replace the BDF below with the one in your setup.

```bash
devlink dev info pci/0000:af:00.0 | grep fw.app
```

```text
        fw.app.name ICE OS Default Package
        fw.app 1.3.59.0
        fw.app.bundle_id 0xc0000001
```

If the version is lower than 1.3.35.0, do [A DDP Package Older Than 1.3.35.0](e800_legacy.md#2-a-ddp-package-older-than-13350).

## 4. Update the Firmware

Do this one time for each adapter.

1. Download the [Intel Ethernet Adapter Complete Driver Pack](https://downloadcenter.intel.com/download/22283/Intel-Ethernet-Adapter-CompleteDriver-Pack).
2. Unpack the NVM update package of your adapter. Replace `<version>` with the versions in the filenames, and `E810` with `E830` for an E830 adapter.

   ```bash
   unzip Release_<version>.zip
   cd NVMUpdatePackage/E810
   tar xvf E810_NVMUpdatePackage_v<version>_Linux.tar.gz
   cd E810/Linux_x64/
   ```

3. Run the update tool. If it shows an update, do the update.

   ```bash
   sudo ./nvmupdate64e
   ```

4. Check the firmware version:

   ```bash
   ethtool -i enp175s0f0 | head -3
   ```

   The `firmware-version` line shows the new version, for example `4.91 0x800214af 1.3909.0` on an E810, or `1.20 0x80017ef4 1.3909.0` on an E830.

## Troubleshooting

For a build error, a module that does not unload, or a missing header, see [Driver Problems](troubleshooting.md#2-driver-problems).

## Next Steps

Go to [the Run Guide](run.md) to set up the host and run a sample application.
