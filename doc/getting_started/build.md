# Build Guide

This guide builds and installs the Media Transport Library (MTL) on Linux. Do the steps in this order:

1. [Get the source code](#1-get-the-source-code).
2. [Install the build packages](#2-install-the-build-packages).
3. [Build and install DPDK](#3-build-and-install-dpdk). MTL uses DPDK for the CPU, memory and NIC queues, so DPDK comes first.
4. [Build and install MTL and the sample applications](#4-build-and-install-mtl-and-the-sample-applications).
5. [Check the build](#5-check-the-build).

Then set up the NIC driver and run a sample: see [Next Steps](#next-steps).

Step 2 is different for each distribution: the script of step 2 runs on Ubuntu only. Steps 3 to 5 are the same on Ubuntu, Debian, CentOS Stream, RHEL 9, Rocky Linux and Arch Linux. For Windows, see the [Windows Build Guide](../platforms/build_WIN.md).

> **One command for steps 2 to 4.** On Ubuntu, `script/mtl_install.sh` can do steps 2, 3 and 4, and also install the NIC driver. It is an alternative to the steps, not an extra step. See [Steps 2 to 4 in One Command](#steps-2-to-4-in-one-command).

## 1. Get the Source Code

```bash
sudo apt-get install -y git         # a fresh Ubuntu may not have it
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
cd Media-Transport-Library
export mtl_source_code=${PWD}
```

Do all the next steps from this folder. The guides use `$mtl_source_code` for it.

## 2. Install the Build Packages

### Ubuntu

```bash
./script/mtl_install_dependencies.sh
```

The script installs the packages of the Debian section below with `sudo`, and some tools that the other scripts use.

### Debian

The script of the Ubuntu section stops on Debian. Install the packages yourself:

```bash
sudo apt-get update
sudo apt-get install git gcc make meson ninja-build python3 python3-pyelftools pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libgmock-dev libssl-dev systemtap-sdt-dev llvm clang gcc-multilib libsdl2-dev libsdl2-ttf-dev patch unzip wget
```

Use the distribution packages of `meson`, `ninja` and `pyelftools`, not `pip`: newer releases block `pip` installs into the system Python (PEP 668), and a `pip install --user meson` breaks `sudo ninja install` (see [Troubleshooting 1.6](troubleshooting.md#16-meson-refuses-the-build-directory)).
The SDL2 packages give RxTxApp a display. Without them, MTL builds with no display.

### CentOS Stream, RHEL, Rocky Linux and Arch Linux

Install the packages of your distribution, as [Packages for Other Distributions](build_advanced.md#3-packages-for-other-distributions) shows. Then continue with step 3.

## 3. Build and Install DPDK

```bash
./script/mtl_build_dpdk.sh
```

The script downloads the DPDK version in `versions.env`, applies the MTL patches of `patches/dpdk/`, builds DPDK, and installs it into `/usr/local` with `sudo`.
It does nothing when that version is already installed. Use `-f` to build it again.

To do the same steps by hand, or to change the build options of DPDK, see [Build DPDK by Hand](build_advanced.md#2-build-dpdk-by-hand).

## 4. Build and Install MTL and the Sample Applications

```bash
./build.sh
sudo ldconfig
```

`build.sh` builds and installs the library, the sample applications of `app/`, RxTxApp, the MtlManager service and the sample plugins.
`sudo ldconfig` lets the runtime linker find the new DPDK and MTL libraries in `/usr/local`.
Without it, an application fails to start with an error such as `libmtl.so: cannot open shared object file`.

## 5. Check the Build

```bash
pkg-config --modversion mtl           # the MTL version, for example 26.09.0
pkg-config --modversion libdpdk       # the DPDK version of versions.env
ls ./tests/tools/RxTxApp/build/RxTxApp ./build/app/
```

If `pkg-config` does not find `mtl` or `libdpdk`, or a step fails, see [Build Problems](troubleshooting.md#1-build-problems).

## Steps 2 to 4 in One Command

`script/mtl_install.sh` runs the scripts of steps 2, 3 and 4 in the correct order. Environment variables select what it does: each one is `0` (off, the default) or `1` (on). This command does steps 2 to 4, and also installs the E810/E830 driver:

```bash
SETUP_ENVIRONMENT=1 \
  SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1 \
  SETUP_BUILD_AND_INSTALL_DPDK=1 \
  MTL_BUILD_AND_INSTALL=1 \
  ./script/mtl_install.sh
sudo ldconfig
```

Leave out `SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1` if your host has no E810 or E830 NIC.
For a setup that you do again, copy the template of the options, set them, and source it:

```bash
cp script/mtl_install_template.env my_setup.env
$EDITOR my_setup.env
source my_setup.env && ./script/mtl_install.sh --dry-run   # show the steps, change nothing
./script/mtl_install.sh
```

`./script/mtl_install.sh -h` tells each option. The [scripts README](../../script/README.md) tells what each script does.

## More Build Options

The [Advanced Build Guide](build_advanced.md) holds the options that the steps above do not need:

* install into a directory of your choice instead of `/usr/local`;
* build DPDK by hand, or with a different number of lcores;
* the packages of CentOS Stream, RHEL 9 and Arch Linux;
* optional components: the E810/E830 driver, the AF_XDP backend, the plugins, FFmpeg, GStreamer, OBS and the Python binding;
* build with clang, build a portable package, and the compiler hardening options.

## Next Steps

1. For an Intel E810 or E830 NIC, check and install its driver: [E800 Series Driver Guide](e800_series_drivers.md). For an Intel I226-V, see the [I226-V Guide](i226.md).
2. Set up the host and run a sample application: [Run Guide](run.md).
