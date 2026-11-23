# Earlier E800 Drivers and DDP Packages

The [E800 Series Driver Guide](e800_series_drivers.md) installs the verified driver, 2.7.6. Use this page only when a host must keep a different driver, or has a DDP package older than 1.3.35.0.
A new installation does not need this page.

## 1. Other Driver Versions

`ethtool -i <interface> | head -3` shows the driver that runs. The Media Transport Library accepts these versions:

| `version` | Driver | Supported |
| --- | --- | --- |
| `2.7.6` | The verified Intel release | Yes. This is the version of `versions.env`. |
| Later than `2.7.6`, for example `2.10.1` | A later Intel release, with no patches | Yes, but we did not verify it. |
| `Kahawai_<version>` | A release before 2.7.6 with the patches of the Media Transport Library | Yes. |
| Before `2.7.6`, with no `Kahawai_` | A release before 2.7.6 with no patches | No: the VFs have no rate limiter. |
| A kernel release, for example `6.8.0-87-generic` | The in-tree driver of the kernel | No: the VFs have no rate limiter. |

`modinfo -F version ice` shows the version of the module that the next load uses. An empty line means the in-tree module.

### 1.1. Driver Versions Before 2.7.6

A driver version before 2.7.6 needs the rate limit patches of the Media Transport Library.
The [ice_drv patch folder](../../patches/ice_drv/) holds one folder for each supported version.

The script applies the patches of the version that you select:

```bash
./script/mtl_build_drivers.sh --driver ice --ice-version <version> --ice-download-id <id>
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

## 2. A DDP Package Older Than 1.3.35.0

The driver of section 2 of the [E800 Series Driver Guide](e800_series_drivers.md#2-install-the-driver) installs a newer DDP package. An older package stays only when a host has an old driver, or when an old package overrides the new one.

Install the package from the `ddp` folder of the driver source tree. If you do not have the tree, get it as [section 2.3 of the driver guide](e800_series_drivers.md#23-other-distributions) shows.

1. Disable the VFs of each E800-series PF. `rmmod ice` fails while a device uses the module. Do the command for each PF, for example for both ports of a card. Then unload `irdma`.

   ```bash
   sudo $mtl_source_code/script/mtl_nicctl.sh disable_vf 0000:af:00.0
   sudo $mtl_source_code/script/mtl_nicctl.sh disable_vf 0000:af:00.1
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

3. Check the DDP version again, as [section 3 of the driver guide](e800_series_drivers.md#3-check-the-ddp-package) shows. If `fw.app` still shows the old version, read `sudo dmesg | tail` for an `ice` message.
   Also look in `/lib/firmware/updates/intel/ice/ddp` for a device-specific `ice-<serial>.pkg`. It overrides the `ice.pkg` link.
4. Refresh the boot image of the running kernel. At early boot, `ice` can load the DDP package from the boot image, which still holds the old package. Use one command:

   ```bash
   # for Ubuntu
   sudo update-initramfs -u -k "$(uname -r)"
   # for CentOS or RHEL
   sudo dracut --force --kver "$(uname -r)"
   ```

5. Create the VFs again on each PF, as [the Run Guide](run.md#4-bind-the-nic-ports-to-dpdk) shows.
