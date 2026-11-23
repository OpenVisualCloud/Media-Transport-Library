# Advanced Build Guide

The [Build Guide](build.md) gives the steps for a new installation. This guide gives the options that those steps do not need.

1. [Install into a predefined directory](#1-install-into-a-predefined-directory), instead of `/usr/local`.
2. [Build DPDK by hand](#2-build-dpdk-by-hand), and set the number of lcores of DPDK.
3. [Packages for other distributions](#3-packages-for-other-distributions): CentOS Stream, RHEL 9, Arch Linux.
4. [Optional components](#4-optional-components): the E810/E830 driver, the AF_XDP backend, the plugins, FFmpeg, GStreamer, OBS, Python.
5. [Build with clang](#5-build-with-clang).
6. [Build a portable package](#6-build-a-portable-package), for a host with a different CPU.
7. [Compiler hardening](#7-compiler-hardening): the options of each build.

## 1. Install into a Predefined Directory

This section is optional. It is not the default setup. If you did the steps of
the [Build Guide](build.md), MTL and its components are already installed into
the system, and you do not need this section.

By default, each build script installs what it builds into `/usr/local`, with
`sudo`. Use this section only when you want to keep the build out of the system.
Set `MTL_INSTALL_PREFIX` to the directory for MTL, and the scripts install into
that directory instead. The path must have no space: DPDK cannot install into such a path. The scripts stay in `script/`. Only the libraries,
programs and plugins that they build go into the directory.

The other components go into directories beside `MTL_INSTALL_PREFIX`, in the
same parent directory. The CI uses `MTL_INSTALL_PREFIX=$PWD/.local_install/mtl`,
which gives these directories:

| Script | Installs into |
| --- | --- |
| `build.sh` | `.local_install/mtl` |
| `script/mtl_build_dpdk.sh` | `.local_install/dpdk` |
| `script/mtl_build_jpegxs.sh` | `.local_install/jpegxs` |
| `script/mtl_build_ffmpeg_plugin.sh` | `.local_install/ffmpeg` |
| `script/mtl_build_gstreamer_plugin.sh` | `.local_install/gstreamer/gstreamer-1.0` |
| `script/mtl_build_drivers.sh` | `.local_install/ice/<kernel release>/<architecture>`. The script only builds the `.ko` file. It does not install or load the driver, because the kernel cannot load a module from this directory. |
| `script/mtl_build_st22_avcodec_plugin.sh` | The directory in `MTL_PLUGIN_PREFIX`, not in `MTL_INSTALL_PREFIX`, for example `.local_install/plugins` |

For example, to install DPDK, MTL and the two ST 2110-22 plugins into `.local_install`:

```bash
export MTL_INSTALL_PREFIX=$PWD/.local_install/mtl
./script/mtl_build_dpdk.sh         # into .local_install/dpdk
./build.sh                     # into .local_install/mtl
./script/mtl_build_jpegxs.sh       # into .local_install/jpegxs, beside them
MTL_PLUGIN_PREFIX=$PWD/.local_install/plugins ./script/mtl_build_st22_avcodec_plugin.sh
```

This directory needs no root, no `ldconfig` and no `LD_LIBRARY_PATH`. Each program
and library records the location of the other libraries relative to itself, and
each `.pc` file points at its own location. So you can move the whole directory,
and it still runs.
`.local_install/mtl/bin/mtl_nicctl.sh` is part of it and uses the `dpdk-devbind.py`
of `.local_install/dpdk`. The plugin registry is `.local_install/conf/kahawai.json`.
Set `KAHAWAI_CFG_PATH` to it, see [plugin.md](../features/plugin.md). The registry holds absolute
paths, so it is the one part of the directory that does not move with it.

These three scripts do not read `MTL_INSTALL_PREFIX` yet. They always install
into the system with `sudo`:

| Script | Installs into | Why it cannot use the directory |
| --- | --- | --- |
| `script/mtl_build_ebpf_xdp.sh` | `/usr/local`, and it links `libxdp.pc` and `libbpf.pc` into the `pkg-config` directory of the system | `build.sh` finds libxdp and libbpf only in the `pkg-config` search path of the system. |
| `script/mtl_build_python.sh` | The `site-packages` directory of the system `python3` | The script reads the MTL headers from `/usr/local/include`, so it also needs MTL in the system. |
| `script/mtl_build_obs_plugin.sh` | The `obs-plugins` directory of the system libobs | The script has no option to select a different directory. |

MTL does not need these three scripts. They add only the AF_XDP data path, the
Python binding and the OBS plugin. If you do not want them in your system, do
not run them. Without them, `build.sh` builds MTL without the AF_XDP backend.

## 2. Build DPDK by Hand

`./script/mtl_build_dpdk.sh` does the steps 2.1 to 2.3 in one command, as step 3 of the [Build Guide](build.md#3-build-and-install-dpdk) shows. Use this section to do them by hand, or to change the DPDK build.

```bash
cd $mtl_source_code
./script/mtl_build_dpdk.sh
```

`mtl_build_dpdk.sh` builds DPDK for the CPU count of the build host, at least the DPDK default of 128 (`max_lcores`, `RTE_MAX_LCORE`): an MTL lcore ID is the CPU ID, so every CPU MTL uses must be below it.
It uses at most 1024, also for a larger `DPDK_MAX_LCORES`: MTL supports CPU IDs 0-1023.
For a Docker or CI image that runs on a host with more CPUs than the build machine, set the count of that host:

```bash
DPDK_MAX_LCORES=512 ./script/mtl_build_dpdk.sh -f
```

`mtl_build_dpdk.sh` does nothing when the same DPDK version is installed already, so a changed `DPDK_MAX_LCORES` needs `-f`.
Build MTL again after DPDK.

Each lcore ID costs 8256 bytes of hugepage memory in every MTL mempool, as each one has a per-lcore cache: a mempool takes 1.0 MiB with the DPDK default of 128, 2.0 MiB with 256, 4.0 MiB with 512 and 8.1 MiB with 1024.
MTL creates a few mempools per port and per session, for example about 160 for 2 redundant ports with 32 TX and 32 RX sessions.
If the host has more CPUs than `RTE_MAX_LCORE` of the installed DPDK, MTL passes `--remap-lcore-ids`, as releases up to 26.09 did, and logs the `DPDK_MAX_LCORES` to rebuild DPDK with: each process then numbers its CPUs from 0, so MTL processes on different CPUs can claim the same lcore.

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
convert the files, use the symlink step in the [Build guide for Windows](../platforms/build_WIN.md).

```bash
cd dpdk
git am $mtl_source_code/patches/dpdk/${DPDK_VER}/*.patch
```

### 2.3. Build and install DPDK library

An MTL lcore ID is the CPU ID, so DPDK must be built with `max_lcores` (`RTE_MAX_LCORE`) above the highest CPU ID of the host where MTL runs.
Use the CPU count of that host, at least the DPDK default of 128 and at most 1024, the most MTL supports; the start of this section lists the hugepage memory it costs.
Build MTL again after DPDK.

```bash
meson setup build -Dmax_lcores=256 # the CPU count of the host, at least 128
ninja -C build
sudo ninja install -C build
cd ..
```

If `meson setup` or `sudo ninja install` fails, see [Build Problems](troubleshooting.md#1-build-problems).

## 3. Packages for Other Distributions

`script/mtl_install_dependencies.sh` installs packages on Ubuntu only. On another distribution, install the packages below, then continue with step 3 of the [Build Guide](build.md#3-build-and-install-dpdk).

### 3.1. CentOS Stream

```bash
sudo yum install -y dnf-plugins-core
sudo dnf config-manager --set-enabled powertools
sudo yum install git gcc gcc-c++ make meson python3 python3-pip pkg-config json-c-devel libpcap-devel gtest-devel openssl-devel numactl-devel libasan systemtap-sdt-devel clang llvm-devel patch unzip wget
sudo pip3 install pyelftools
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo yum install SDL2-devel
```

### 3.2. RHEL 9 and Rocky Linux 9

```bash
sudo yum install git gcc gcc-c++ make python3 python3-pip pkg-config SDL2-devel openssl-devel numactl-devel libasan systemtap-sdt-devel patch unzip wget
sudo pip3 install meson ninja pyelftools
```

RHEL 9 doesn't provide `json-c-devel libpcap-devel gtest-devel` package as default, it has to build these three libs from source code, install below dependency and follow section [3.5. Build Dependencies from Source Code](#35-build-dependencies-from-source-code) to build.

```bash
sudo yum install cmake flex bison autoconf
```

### 3.3. The Library Path of CentOS Stream, RHEL and Rocky Linux

DPDK and MTL install their libraries into `/usr/local/lib64`, which the runtime linker of these distributions does not search. Do this one time, after step 4 of the [Build Guide](build.md#4-build-and-install-mtl-and-the-sample-applications):

```bash
echo /usr/local/lib64 | sudo tee /etc/ld.so.conf.d/mtl-local.conf
sudo ldconfig
```

Without it, RxTxApp and the samples run, because they hold the path, but your own application gives `libmtl.so: cannot open shared object file`.

### 3.4. Arch Linux

```bash
sudo pacman -Syu --needed git gcc make meson python python-pyelftools pkg-config json-c libpcap gtest openssl numactl patch unzip wget
```

Install below SDL2 packages if you want the display support for RxTxApp.

```bash
sudo pacman -Syu --needed  sdl2 sdl2_ttf
```

### 3.5. Build Dependencies from Source Code

It's true that not all operating systems, including RHEL 9, come with all the libraries required for every software package. If you're trying to install a software that has dependencies not provided by default by your package manager, you might need to install these dependencies manually. Skip these step if your setup has all dependencies resolved.

Refer to below commands for how to build from code.

#### 3.5.1. json-c

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

#### 3.5.2. libpcap

```bash
git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-1.10.7
cd libpcap/
./autogen.sh
./configure
make
sudo make install
cd ..
```

#### 3.5.3. gtest

```bash
git clone https://github.com/google/googletest.git -b v1.13.x
cd googletest/
mkdir build
cd build/
cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON ../
make
sudo make install
cd ../../
```

`CMAKE_POSITION_INDEPENDENT_CODE=ON` is necessary: MTL links the test programs as PIE, and a static `libgtest.a` without it gives `relocation R_X86_64_32 against .rodata can not be used when making a PIE object`.

### 3.6. secure_path for the root User

The build steps use `sudo ninja install` to install the build to the system. Some operating systems, including CentOS Stream and RHEL 9, do not have `/usr/local/bin/` in the secure_path defaults.

Edit the file `/etc/sudoers`, find `secure_path` and append `/usr/local/bin`

```bash
Defaults    secure_path = /sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
```

Reboot the system to let the change take effect.

Then continue with step 3 of the [Build Guide](build.md#3-build-and-install-dpdk).

## 4. Optional Components

Some setups need more components. Add each component to `mtl_install_dependencies.sh`, which then installs its build packages too:

| Component | Packages | Build and install |
| --- | --- | --- |
| Intel E810/E830 out-of-tree driver | `./script/mtl_install_dependencies.sh ice` | `./script/mtl_build_drivers.sh --driver ice`, see the [E800 Series Driver Guide](e800_series_drivers.md) |
| AF_XDP data path (xdp-tools and libbpf), experimental | `./script/mtl_install_dependencies.sh ebpf_xdp` | `./script/mtl_build_ebpf_xdp.sh`, see the [XDP guide](../experimental/xdp.md) |
| SVT JPEG XS ST 2110-22 plugin | `./script/mtl_install_dependencies.sh jpegxs` | `./script/mtl_build_jpegxs.sh`, see the [Plugin Guide](../features/plugin.md) |
| ST 2110-22 avcodec plugin | `./script/mtl_install_dependencies.sh avcodec` | `./script/mtl_build_st22_avcodec_plugin.sh`, see the [avcodec plugin guide](../../plugins/st22_avcodec/README.md) |
| FFmpeg with the MTL devices | `./script/mtl_install_dependencies.sh ffmpeg` | `./script/mtl_build_ffmpeg_plugin.sh`, see the [FFmpeg plugin guide](../../ecosystem/ffmpeg_plugin/README.md) |
| GStreamer plugins | `./script/mtl_install_dependencies.sh gstreamer` | `./script/mtl_build_gstreamer_plugin.sh`, see the [GStreamer plugin guide](../../ecosystem/gstreamer_plugin/README.md) |
| Python binding | `./script/mtl_install_dependencies.sh python` | `./script/mtl_build_python.sh`, see the [Python guide](../../python/README.md) |
| OBS Studio plugin | `./script/mtl_install_dependencies.sh obs` | `./script/mtl_build_obs_plugin.sh`, see the [OBS plugin guide](../../ecosystem/obs_mtl/README.md) |

The AF_XDP and kernel socket backends are experimental. The nightly tests run them, but their pacing is less accurate than that of the DPDK PMD, and they are not the recommended setup. The [experimental](../experimental/) folder holds their guides. They also need DPDK, for the CPU, memory and queue functions of MTL.
Install the AF_XDP components before you run `./build.sh`: MTL finds libxdp and libbpf when it is configured, and without them builds no AF_XDP backend.
Build MTL with `./build.sh` before you build a plugin or the Python binding.

## 5. Build with clang

Before build, export CC/CXX to clang, see below for example.

```bash
export CC=clang CXX=clang++
rm build -rf
./build.sh
```

## 6. Build a Portable Package

A portable package runs on other hosts too. The default DPDK build of [section 2.3](#23-build-and-install-dpdk-library) uses `-march=native`.
This generates native instructions based on local CPU features, some of which may not exist on other devices. In such scenarios, it's necessary to specifically indicate the CPU instructions you want to support.

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

## 7. Compiler Hardening

The Linux build compiles the MTL meson projects with the hardening options below, and DPDK, libbpf, libxdp, openh264, FFmpeg, the MTL plugin of SVT-JPEG-XS and the Level Zero loader of gpu_direct with the options for C, less the warning options: openh264 and Level Zero are C++, but get no `-D_GLIBCXX_ASSERTIONS`.
The `meson.build` of each project adds them with `add_project_arguments()` and `add_project_link_arguments()`, and `script/mtl_build_dpdk.sh`, `script/mtl_build_ebpf_xdp.sh`, `script/mtl_build_ffmpeg_plugin.sh`, `script/mtl_build_jpegxs.sh` and `script/mtl_install_dependencies.sh gpu_direct` pass them to the dependencies they build, from `hardening_flags()` in `script/mtl_common.sh`.
In the dependency builds `hardening_flags()` applies the `_FORTIFY_SOURCE` rule of the meson projects: it adds no level when `CFLAGS` or `CPPFLAGS` set one or a sanitizer (`-fsanitize=`), or when the last `-O` of `CFLAGS`, then `CPPFLAGS`, is `-O0`, as `b_sanitize` and the meson option `optimization` at `0` do for the MTL projects.
`MTL_HARDENING=0` builds them without the flags, and DPDK and the FFmpeg programs without PIE, for a build that sets its own; the scripts say so in a warning.

| Option | Protection |
| --- | --- |
| `-fstack-protector-strong` | Stack buffer overflow canary |
| `-fstack-clash-protection` | Stack clash |
| `-fcf-protection=full` | Intel CET shadow stack (SHSTK) and indirect branch tracking (IBT) |
| `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3`, or `=2` | Buffer overflow checks in libc calls. Level 3 needs gcc 12 or clang 9 and glibc 2.34, so gcc 11 (Ubuntu 22.04, RHEL 9) gets 2. Not added when `c_args` or `cpp_args` set a level, with the meson option `optimization` at `0` or `plain` (buildtypes `debug`, `plain`), or with `b_sanitize`, which the sanitizers don't support |
| `-Wformat -Wformat-security -Werror=format-security` | Format string attacks |
| `-Wimplicit-fallthrough` | A `switch` case that falls through unmarked. Mark an intended one with `__attribute__((fallthrough));`, as clang ignores a `/* fallthrough */` comment. DPDK's headers, which use the comment, are included as system headers |
| `-D_GLIBCXX_ASSERTIONS` | Bounds and precondition checks in libstdc++, for the C++ programs: MtlManager, KahawaiTest, UnitTest, the gpu_direct gtest and the st22 avcodec and FFmpeg plugin tests |
| `-Wl,-z,relro -Wl,-z,now` | Full RELRO, a read-only GOT |
| `-Wl,-z,noexecstack` | Non-executable stack |
| `b_pie=true`, and meson's `-fPIC` for shared objects | Position independent code for ASLR |

The `-U_FORTIFY_SOURCE` removes the level that Ubuntu gcc sets by itself, so `-Werror` does not stop the build on a redefinition, and the level that the toolchain applies takes its place: 3 with gcc 12 and later and with clang, 2 with gcc 11 (Ubuntu 22.04, RHEL 9, Rocky Linux 9), which cannot apply 3. A level that `c_args` or `cpp_args` already set, also through `CFLAGS` or `CPPFLAGS`, is kept.

The gcc of Rocky Linux and RHEL enables none of the compiler options by default. So the Rocky Linux 9 image build runs `.github/scripts/ci/scan/check-hardening.sh` on what `build.sh` builds there: libmtl, UnitTest, the samples, the sample plugins, MtlManager, KahawaiTest and RxTxApp. It fails on a binary without full RELRO, a non-executable stack, PIE, CET or the stack protector.

The FFmpeg 7.0 assembly is marked SHSTK only, so the build applies `ecosystem/ffmpeg_plugin/7.0/0002-x86-add-Intel-CET-IBT-support.patch`, which makes it IBT-correct and marks it IBT and SHSTK.
