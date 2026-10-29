# Build Guide

Building the Media Transport Library requires three parts: building the DPDK library, building the Media Transport Library on top of DPDK, and building the sample application.

Please note that the DPDK dependency remains necessary when utilizing the XDP/kernel socket data path backend. We leverage numerous DPDK APIs, including those for CPU, memory, queues, and mbufs, to construct a highly efficient network processing implementation.

You can do the setup in two ways:

* With the setup scripts in the `script/` folder. On Ubuntu, they do sections 1 to 3 for you. See [Quick Start with the Setup Scripts](#quick-start-with-the-setup-scripts).
* Step by step, with the commands in sections 1 to 3. Use this way on other operating systems, or when you want to control each step.

## Quick Start with the Setup Scripts

Run the scripts as your own user, from the top of the source tree. They use `sudo` when they install to the system.
Each script prints its options with `-h`.

```bash
sudo apt-get install -y git         # a fresh Ubuntu may not have it yet
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
cd Media-Transport-Library
./script/install_dependencies.sh    # the build packages of section 1.1.1, Ubuntu only
./script/build_dpdk.sh              # section 2: download, patch, build and install DPDK
./build.sh                          # section 3: build and install MTL and the sample apps
sudo ldconfig                       # section 3: let the loader find the new libraries
```

`build_dpdk.sh` builds the DPDK version in `versions.env`. It does nothing if that version is already installed. Use `-f` to rebuild it.

Some setups need more components. Add each component to `install_dependencies.sh`, which then installs its build packages too:

| Component | Packages | Build and install |
| --- | --- | --- |
| Intel E810/E830 driver with the MTL patches | `./script/install_dependencies.sh ice` | `./script/build_drivers.sh --driver ice`, see the [E800 Series Driver Guide](e800_series_drivers.md) |
| AF_XDP data path (xdp-tools and libbpf) | `./script/install_dependencies.sh ebpf_xdp` | `./script/build_ebpf_xdp.sh`, see the [XDP guide](xdp.md) |
| SVT JPEG XS ST 2110-22 plugin | `./script/install_dependencies.sh jpegxs` | `./script/build_jpegxs.sh`, see the [Plugin Guide](plugin.md) |
| ST 2110-22 avcodec plugin | `./script/install_dependencies.sh avcodec` | `./script/build_st22_avcodec_plugin.sh`, see the [avcodec plugin guide](../plugins/st22_avcodec/README.md) |
| FFmpeg with the MTL devices | `./script/install_dependencies.sh ffmpeg` | `./script/build_ffmpeg_plugin.sh`, see the [FFmpeg plugin guide](../ecosystem/ffmpeg_plugin/README.md) |
| GStreamer plugins | `./script/install_dependencies.sh gstreamer` | `./script/build_gstreamer_plugin.sh`, see the [GStreamer plugin guide](../ecosystem/gstreamer_plugin/README.md) |
| Python binding | `./script/install_dependencies.sh python` | `./script/build_python.sh`, see the [Python guide](../python/README.md) |
| OBS Studio plugin | `./script/install_dependencies.sh obs` | `./script/build_obs_plugin.sh`, see the [OBS plugin guide](../ecosystem/obs_mtl/README.md) |

Install the AF_XDP components before you run `./build.sh`: MTL finds libxdp and libbpf when it is configured, and without them builds no AF_XDP backend.
Build MTL with `./build.sh` before you build a plugin or the Python binding.

### Install into a predefined directory

This section is optional. It is not the default setup. If you did the steps of
the [quick start](#quick-start-with-the-setup-scripts) above, MTL and its
components are already built and installed into the system. You can skip this
section.

By default, each build script installs what it builds into `/usr/local`, with
`sudo`. Use this section only when you want to keep the build out of the system.
Set `MTL_INSTALL_PREFIX` to the directory for MTL, and the scripts install into
that directory instead. The scripts stay in `script/`. Only the libraries,
programs and plugins that they build go into the directory.

The other components go into directories beside `MTL_INSTALL_PREFIX`, in the
same parent directory. The CI uses `MTL_INSTALL_PREFIX=$PWD/.local_install/mtl`,
which gives these directories:

| Script | Installs into |
| --- | --- |
| `build.sh` | `.local_install/mtl` |
| `script/build_dpdk.sh` | `.local_install/dpdk` |
| `script/build_jpegxs.sh` | `.local_install/jpegxs` |
| `script/build_ffmpeg_plugin.sh` | `.local_install/ffmpeg` |
| `script/build_gstreamer_plugin.sh` | `.local_install/gstreamer/gstreamer-1.0` |
| `script/build_drivers.sh` | `.local_install/ice/<kernel release>/<architecture>`. The script only builds the `.ko` file. It does not install or load the driver, because the kernel cannot load a module from this directory. |
| `script/build_st22_avcodec_plugin.sh` | The directory in `MTL_PLUGIN_PREFIX`, not in `MTL_INSTALL_PREFIX`, for example `.local_install/plugins` |

For example, to install DPDK, MTL and the two ST 2110-22 plugins into `.local_install`:

```bash
export MTL_INSTALL_PREFIX=$PWD/.local_install/mtl
./script/build_dpdk.sh         # into .local_install/dpdk
./build.sh                     # into .local_install/mtl
./script/build_jpegxs.sh       # into .local_install/jpegxs, beside them
MTL_PLUGIN_PREFIX=$PWD/.local_install/plugins ./script/build_st22_avcodec_plugin.sh
```

This directory needs no root, no `ldconfig` and no `LD_LIBRARY_PATH`. Each program
and library records the location of the other libraries relative to itself, and
each `.pc` file points at its own location. So you can move the whole directory,
and it still runs.
`.local_install/mtl/bin/nicctl.sh` is part of it and uses the `dpdk-devbind.py`
of `.local_install/dpdk`. The plugin registry is `.local_install/conf/kahawai.json`.
Set `KAHAWAI_CFG_PATH` to it, see [plugin.md](./plugin.md). The registry holds absolute
paths, so it is the one part of the directory that does not move with it.

These three scripts do not read `MTL_INSTALL_PREFIX` yet. They always install
into the system with `sudo`:

| Script | Installs into | Why it cannot use the directory |
| --- | --- | --- |
| `script/build_ebpf_xdp.sh` | `/usr/local`, and it links `libxdp.pc` and `libbpf.pc` into the `pkg-config` directory of the system | `build.sh` finds libxdp and libbpf only in the `pkg-config` search path of the system. |
| `script/build_python.sh` | The `site-packages` directory of the system `python3` | The script reads the MTL headers from `/usr/local/include`, so it also needs MTL in the system. |
| `script/build_obs_plugin.sh` | The `obs-plugins` directory of the system libobs | The script has no option to select a different directory. |

MTL does not need these three scripts. They add only the AF_XDP data path, the
Python binding and the OBS plugin. If you do not want them in your system, do
not run them. Without them, `build.sh` builds MTL without the AF_XDP backend.

### Set up the host with one command

`.github/scripts/setup_environment.sh` runs the scripts above in the correct order. Environment variables select the steps. Each variable is `0` (the default) or `1`. The command below installs the packages, the patched E810/E830 driver, DPDK, and MTL:

```bash
SETUP_ENVIRONMENT=1 \
  SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1 \
  SETUP_BUILD_AND_INSTALL_DPDK=1 \
  MTL_BUILD_AND_INSTALL=1 \
  ./.github/scripts/setup_environment.sh
sudo ldconfig
```

Leave out `SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1` if your setup has no E810/E830 NIC.
`SETUP_BUILD_AND_INSTALL_DPDK=1` rebuilds DPDK on each run, also when the version is already installed.
Run `./.github/scripts/setup_environment.sh -h` for the list of all variables, for example `SETUP_BUILD_AND_INSTALL_EBPF_XDP`, `PLUGIN_BUILD_AND_INSTALL_JPEGXS`, or `ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN`.

The scripts do not set up the runtime: hugepages, VFs, and MtlManager. For that, refer to [Running MTL](./run.md).

## 1. Prerequisites

### 1.1. Install the build dependency from OS software store

#### 1.1.1. Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install git gcc meson ninja-build python3 python3-pyelftools pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libgmock-dev libssl-dev systemtap-sdt-dev llvm clang flex byacc
```

Newer Ubuntu and Debian releases enable Python's externally managed environment
protection (PEP 668), so prefer the distribution packages above instead of
installing `pyelftools` or `ninja` with `pip`.

On Ubuntu, `./script/install_dependencies.sh` installs these packages from the distribution, and the SDL2 packages below.
It leaves out `flex` and `byacc`, which only [section 1.2](#12-build-dependency-from-source-code) needs.

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo apt-get install libsdl2-dev libsdl2-ttf-dev
```

#### 1.1.2. CentOS stream

```bash
sudo yum install -y dnf-plugins-core
sudo dnf config-manager --set-enabled powertools
sudo yum install git gcc gcc-c++ meson python3 python3-pip pkg-config json-c-devel libpcap-devel gtest-devel openssl-devel numactl-devel libasan systemtap-sdt-devel clang llvm-devel
sudo pip3 install pyelftools
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo yum install SDL2-devel
```

#### 1.1.3. RHEL 9

```bash
sudo yum install git gcc gcc-c++ python3 python3-pip pkg-config SDL2-devel openssl-devel numactl-devel libasan systemtap-sdt-devel
sudo pip3 install meson ninja pyelftools
```

RHEL 9 doesn't provide `json-c-devel libpcap-devel gtest-devel` package as default, it has to build these three libs from source code, install below dependency and follow section [1.2. Build dependency from source code](#12-build-dependency-from-source-code) to build.

```bash
sudo yum install cmake flex bison autoconf
```

#### 1.1.4. Arch Linux

```bash
sudo pacman -Syu --needed git gcc meson python python-pyelftools pkg-config json-c libpcap gtest openssl numactl
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo pacman -Syu --needed  sdl2 sdl2_ttf
```

### 1.2. Build dependency from source code

It's true that not all operating systems, including RHEL 9, come with all the libraries required for every software package. If you're trying to install a software that has dependencies not provided by default by your package manager, you might need to install these dependencies manually. Skip these step if your setup has all dependencies resolved.

Refer to below commands for how to build from code.

#### 1.2.1. json-c

```bash
git clone https://github.com/json-c/json-c.git -b json-c-0.16
cd json-c/
mkdir build
cd build
cmake ../
make
sudo make install
cd ../../
```

#### 1.2.2. libpcap

```bash
git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-1.10.7
cd libpcap/
./autogen.sh
./configure
make
sudo make install
cd ..
```

#### 1.2.3. gtest

```bash
git clone https://github.com/google/googletest.git -b v1.13.x
cd googletest/
mkdir build
cd build/
cmake ../
make
sudo make install
cd ../../
```

### 1.3. secure_path for root user

The build steps use `sudo ninja install` to install the build to the system. Some operating systems, including CentOS Stream and RHEL 9, do not have `/usr/local/bin/` in the secure_path defaults.

Edit the file `/etc/sudoers`, find `secure_path` and append `/usr/local/bin`

```bash
Defaults    secure_path = /sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
```

Reboot the system to let change take effect.

### 1.4. Clone Media Transport Library code

Download Media Transport Library to top folder Directory

```bash
git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
export mtl_source_code=${PWD}/Media-Transport-Library
```

## 2. DPDK build and install

`./script/build_dpdk.sh` does the steps 2.1 to 2.3 in one command. It downloads the DPDK version in `versions.env`, applies the patches, then builds and installs DPDK.

```bash
cd $mtl_source_code
./script/build_dpdk.sh
```

### 2.1. Get the DPDK source code

`versions.env` in the Media Transport Library source tree holds the DPDK version to use. Read the file to set `DPDK_VER`, then check out that version.

```bash
. $mtl_source_code/versions.env
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v${DPDK_VER}
git switch -c v${DPDK_VER}
cd ..
```

### 2.2. Apply the DPDK patches required to run Media Transport Library

Note: point `$mtl_source_code` to the top source code tree of Media Transport Library.

The patch folder holds one folder per DPDK version. `DPDK_VER` from step 2.1 selects the correct one.

Some patches in the DPDK versions 22.03 to 23.11 are symlinks to a patch of an older version.
They resolve without an extra step. If your clone is on a file system that cannot hold symlinks,
Git writes each one as a text file that holds the target path, and `git am` rejects it. To
convert the files, use the symlink step in the [Build guide for Windows](build_WIN.md).

```bash
cd dpdk
git am $mtl_source_code/patches/dpdk/${DPDK_VER}/*.patch
```

### 2.3. Build and install DPDK library

```bash
meson setup build
ninja -C build
sudo ninja install -C build
cd ..
```

Note, please make sure libnuma is installed, confirm it from the log of `meson setup build` command.

```text
Library numa found: YES
```

If you see below log from `sudo ninja install -C build`, it seems you're encountering an issue where the ninja command isn't recognized. This problem is likely due to the ninja executable not being in your system's PATH, refer to `### 1.3`

```bash
sudo: ninja: command not found
```

## 3. Build Media Transport Library and app

```bash
cd $mtl_source_code
./build.sh
sudo ldconfig
```

`sudo ldconfig` updates the cache of the runtime linker, so it finds the DPDK and MTL libraries in `/usr/local`. meson 0.62 and later do not run it after an install. Without it, an app fails to start with an error such as `libmtl.so: cannot open shared object file`.

### 3.1. Build flow for Intel I226-V (igc)

Intel I226-V can be used with MTL, but the setup is different from Intel E810-focused
environments:

* Use the in-tree Linux `igc` driver for normal host networking and PTP validation.
* Use DPDK `igc` PMD if you plan to run MTL with DPDK backend.

The script skips this step when the `igc` module is already installed. Otherwise, it
installs the running kernel's module package from the configured OS repository.

### 3.2. I226-V (`igc` PMD) notes from DPDK driver guidance

No extra MTL compile-time flag is required specifically for I226-V. The key updates are in
runtime setup and validation:

* Ensure your DPDK build includes the `igc` PMD and that `libdpdk` is installed correctly.
* Use `vfio-pci` binding for DPDK datapath runs (kernel `igc` is still fine for non-DPDK paths).
* Confirm IOMMU and hugepages are enabled before running MTL with DPDK backend.

Typical post-build checks for I226-V:

```bash
# verify NIC and active kernel driver
lspci -nn | grep -i -E "ethernet|intel"

# check DPDK sees the device and driver bindings
sudo dpdk-devbind.py -s

# verify PMD is present in your DPDK test binary
dpdk-testpmd --help
```

## 4. FAQ

### 4.1. PKG_CONFIG_PATH issue

It may get below error caused by PKG_CONFIG_PATH path problem.

```text
lib/meson.build:10:0: ERROR: Dependency "libdpdk" not found, tried pkgconfig
```

Try below command to find the pc path and add to the PKG_CONFIG_PATH env.

```bash
find / -name libdpdk.pc
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/:/usr/local/lib/pkgconfig/
```

### 4.2. Build with clang

Before build, export CC/CXX to clang, see below for example.

```bash
export CC=clang CXX=clang++
rm build -rf
./build.sh
```

### 4.3. Build portable package

Sometimes, you may need to create a portable package that can be used directly on other nodes. Please note that the default steps in section `### 2.3` utilize the `-march=native` compiler version. This generates native instructions based on local CPU features, some of which may not exist on other devices. In such scenarios, it's necessary to specifically indicate the CPU instructions you want to support.

Below are the steps to generate a DPDK binary capable of AVX2. Set `cpu_instruction_set` to `haswell` during the DPDK meson setup stage.

```bash
cd dpdk
rm build -rf
meson setup build -Dcpu_instruction_set=haswell
ninja -C build
sudo ninja install -C build
cd ..
```

Next, rebuild the Media Transport Library. MTL will reuse the build flags from DPDK.

```bash
cd $mtl_source_code
rm build -rf
./build.sh
```

If you want to enable a AVX512 capable build, just set `cpu_instruction_set` to `skylake-avx512`:

```bash
meson setup build -Dcpu_instruction_set=skylake-avx512
```

Use the command below to check the cflags (march) used for the DPDK build:

```bash
pkg-config --cflags libdpdk
# below output indicate it use haswell arch
-include rte_config.h -march=haswell -msse4 -I/usr/local/include
```

Use below command to check the detail compiler flags of one `march`:

```bash
echo | gcc -dM -E - -march=haswell
```

### 4.4. Older kernel installed on the OS

In case older kernel installed on the OS throws the “Kernel header files not in any of expected locations”
and causes build errors, you can try to fix it with installation of kernel specific packet `kernel-devel`:

```bash
# e.g. for CentOS or RHEL
sudo yum install "kernel-devel-$(uname -r)"
```

or with:
```bash
# e.g. for Ubuntu
sudo apt-get install linux-headers-$(uname -r)
```

For older kernel version on Red Hat, the issue is that Red Hat uses vault repos. Older versions are stored in the vaults and the correct repository needs to be enabled to install the targeted version.

[The exemplary location for Rocky Linux](https://dl.rockylinux.org/vault/rocky/)

[The exemplary location for the kernel-devel package for Rocky Linux 9.3](https://dl.rockylinux.org/vault/rocky/9.3/BaseOS/x86_64/os/Packages/k/)

## Next Steps
Proceed to [Running MTL](./run.md) for further instructions.
