# E800 Series Driver Guide

Driver setup for Intel® E810 and E830 Series Ethernet Adapters.

The Media Transport Library needs these parts on the host:

| Part | Version | Section |
| --- | --- | --- |
| Intel `ice` driver | 2.7.6 or later, as Intel releases it. `ICE_VER` in `versions.env` is the version that we verified. | [1](#1-check-the-driver), [2](#2-install-the-driver) |
| DDP package | 1.3.35.0 or later | [3](#3-check-the-ddp-package) |
| NVM firmware | The latest release | [4](#4-update-the-firmware) |

From version 2.7.6, the driver needs no patches of the Media Transport Library.

The `ice` module that comes with the Linux kernel (the in-tree driver) is not sufficient.
It gives a VF no rate limiter. The rate-limit pacing of the library then stops the application with a segmentation fault in `iavf_tm_node_add`.
Intel releases the driver only as source code, so you build it for your kernel. One script does all the steps.

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

The `version` line tells which driver runs:

| `version` | Driver | What to do |
| --- | --- | --- |
| `2.7.6` or later | An Intel release | Nothing. Go to [Section 3](#3-check-the-ddp-package). |
| `Kahawai_<version>` | An earlier release with the patches of the Media Transport Library | The library supports it. To change to the verified version, do [Section 2](#2-install-the-driver). |
| An earlier version with no `Kahawai_` | An earlier release with no patches | Do [Section 2](#2-install-the-driver). |
| The kernel release, for example `6.8.0-87-generic` | The in-tree driver of the kernel | Do [Section 2](#2-install-the-driver). |

`modinfo -F version ice` shows the version of the module that the next load uses. An empty line means the in-tree module.

## 2. Install the Driver

Run the commands from the top of the Media Transport Library source tree.

```bash
./script/install_dependencies.sh ice
./script/build_drivers.sh --driver ice
```

`install_dependencies.sh ice` installs the build tools and the headers of the running kernel. It runs only on Ubuntu.
On another distribution, install `gcc`, `make`, `tar`, `gzip` and `wget` yourself, and the kernel headers as [Section 5.1](#51-the-kernel-header-files-are-missing) shows.
`build_drivers.sh` downloads the driver version in `versions.env`, builds it, installs it with its DDP package, and loads it.
It does nothing when that version is already installed for the running kernel. Use `--force` to build it again.

The reload of the driver removes the VFs of each E800-series PF:

- Before you run the script, stop each application that uses a VF of an E800-series PF.
- After the script, create the VFs again, as [the Run Guide](run.md#321-create-intel-e800-series-vfs-and-bind-to-dpdk-pmd) shows.

Then do [Section 1](#1-check-the-driver) again. The `version` line shows the version in `versions.env`.

> **Note:** A new kernel needs a new build of the driver. Ubuntu can install a new kernel, and boot it at the next reboot. Run `./script/build_drivers.sh --driver ice` again after each kernel upgrade.

### 2.1. Install Without the Script

`$mtl_source_code` is the top of the Media Transport Library source tree.

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
```

`make install` installs the module and the DDP package of the source tree, and refreshes the boot image.
The source code is also on GitHub, as the tag `v<version>` of [intel/ethernet-linux-ice](https://github.com/intel/ethernet-linux-ice).

The Intel download page can show a newer driver version. Use the version in `versions.env`, because that is the version that we verified. We examine new driver versions each quarter.

### 2.2. Driver Versions Before 2.7.6

A driver version before 2.7.6 needs the rate limit patches of the Media Transport Library.
The [ice_drv patch folder](../../patches/ice_drv/) holds one folder for each supported version.

The script applies the patches of the version that you select:

```bash
./script/build_drivers.sh --driver ice --ice-version <version> --ice-download-id <id>
```

`<id>` is the number in the Intel download URL of that version. Without a correct ID, the script downloads the source code from GitHub.

To apply the patches by hand, do this in the driver source tree before `make`:

```bash
git init
git add .
git commit -m "init version <version>"
git am $mtl_source_code/patches/ice_drv/<version>/*.patch
```

A patched driver shows `Kahawai_<version>` as its version.

## 3. Check the DDP Package

The Media Transport Library needs DDP version 1.3.35.0 or later.
Section 2 installs the DDP package of the driver together with the module.

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

If the version is lower than 1.3.35.0, do [Section 5.3](#53-the-ddp-package-is-older-than-13350).

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

4. Do [Section 1](#1-check-the-driver) again. The `firmware-version` line shows the new version, for example `4.91 0x800214af 1.3909.0` on an E810, or `1.20 0x80017ef4 1.3909.0` on an E830.

## 5. Troubleshooting

### 5.1. The Kernel Header Files Are Missing

If `make` shows this error, the header files of the running kernel are missing:

```text
*** Kernel header files not in any of the expected locations.
```

Install them:

```bash
# for Ubuntu
sudo apt-get install linux-headers-$(uname -r)
# for CentOS or RHEL
sudo yum install kernel-devel-$(uname -r)
```

### 5.2. The ice Module Is in Use by irdma

If `rmmod ice` shows this error, run `sudo rmmod irdma`, then do the command again:

```text
rmmod: ERROR: Module ice is in use by: irdma
```

The Media Transport Library does not use `irdma`, so leave it unloaded.
If something else on this host needs RDMA, run `sudo modprobe irdma`.

### 5.3. The DDP Package Is Older Than 1.3.35.0

Install the package from the `ddp` folder of the driver source tree. If you do not have the tree, get it as Section 2.1 shows.

1. Disable the VFs of each E800-series PF. `rmmod ice` fails while a device uses the module. Do the command for each PF, for example for both ports of a card. Then unload `irdma`.

   ```bash
   sudo $mtl_source_code/script/nicctl.sh disable_vf 0000:af:00.0
   sudo $mtl_source_code/script/nicctl.sh disable_vf 0000:af:00.1
   sudo rmmod irdma
   ```

2. From the top of the driver source tree, copy the package and load the driver again. The commands read the name of the package from the `ddp` folder.

   ```bash
   DDP_PKG=$(ls ddp/ice-*.pkg) && \
   sudo mkdir -p /lib/firmware/updates/intel/ice/ddp && \
   sudo cp "${DDP_PKG}" /lib/firmware/updates/intel/ice/ddp/ && \
   sudo ln -sf "$(basename "${DDP_PKG}")" /lib/firmware/updates/intel/ice/ddp/ice.pkg && \
   sudo rmmod ice
   sudo modprobe ice
   ```

3. Do [Section 3](#3-check-the-ddp-package) again. If `fw.app` still shows the old version, read `sudo dmesg | tail` for an `ice` message.
   Also look in `/lib/firmware/updates/intel/ice/ddp` for a device-specific `ice-<serial>.pkg`. It overrides the `ice.pkg` link.
4. Refresh the boot image of the running kernel. At early boot, `ice` can load the DDP package from the boot image, which still holds the old package. Use one command:

   ```bash
   # for Ubuntu
   sudo update-initramfs -u -k "$(uname -r)"
   # for CentOS or RHEL
   sudo dracut --force --kver "$(uname -r)"
   ```

5. Create the VFs again on each PF, as [the Run Guide](run.md#321-create-intel-e800-series-vfs-and-bind-to-dpdk-pmd) shows.

## Next Steps

Go to [the Run Guide](run.md#3-dpdk-pmd-setup) for the next instructions.
