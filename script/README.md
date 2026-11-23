# MTL Scripts

The scripts of this folder set up a Linux host for the Media Transport Library
(MTL): they install the build packages, build and install DPDK, the NIC
drivers, MTL and its plugins, and they set up and check the NICs, the DMA
devices and the CPU isolation of a running host.

The name of each script starts with `mtl_`. Each script prints its full help
with `-h`: the commands, the options, the packages that it needs, the
environment variables that it reads, and examples. This page gives only the
overview. The help of the script is the reference.

## Rules for all scripts

- Run each setup and build script as your own user, from any directory. It
  uses `sudo` for each command that needs root. Run a host tool that changes
  the host with `sudo`: the "Root" column of the tables tells which.
- The build scripts install into `/usr/local` by default. Set
  `MTL_INSTALL_PREFIX` to install into a directory of your choice instead, see
  [Install into a Predefined Directory](../doc/getting_started/build_advanced.md#1-install-into-a-predefined-directory).
- `mtl_install_dependencies.sh`, and `mtl_install.sh` with
  `SETUP_ENVIRONMENT=1`, install packages on Ubuntu only. The build scripts
  run on each distribution that has the packages: Ubuntu 22.04, 24.04 and
  26.04, Debian 12 and Rocky Linux 9 are tested. On another distribution,
  install the packages as section 3 of the
  [Advanced Build Guide](../doc/getting_started/build_advanced.md#3-packages-for-other-distributions)
  shows.
- `versions.env` in the top folder holds the version of each component (DPDK,
  ice, FFmpeg, JPEG XS, xdp-tools). The scripts read it. Do not give a version
  on the command-line unless you need a different one.
- You can source each script in a shell: then it defines its functions and
  does nothing. Tests use this.

## Set up a host with one command: `mtl_install.sh`

`mtl_install.sh` runs the other scripts in the correct order. Environment
variables select the steps: each variable is `0` (off, the default) or `1`
(on). The easy way is the template:

```bash
cp script/mtl_install_template.env my_setup.env
$EDITOR my_setup.env                   # set each option that you need to 1
source my_setup.env
./script/mtl_install.sh --dry-run      # show the steps, change nothing
./script/mtl_install.sh                # do the steps
sudo ldconfig                          # only for an install into the system
```

As it comes, the template installs the build packages, DPDK and MTL. Set
`SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1` for an E810 or E830 NIC.

You can also give the options on the command line:

```bash
SETUP_ENVIRONMENT=1 SETUP_BUILD_AND_INSTALL_DPDK=1 MTL_BUILD_AND_INSTALL=1 ./script/mtl_install.sh
```

The script stops before it does any work when a value is not `0` or `1`, or
when a variable looks like an option but is not one, for example
`SETUP_BUILD_AND_INSTALL_DPKD=1`. `./script/mtl_install.sh -h` gives each
option and what it does.

`mtl_install.sh` does not set up the runtime: hugepages, VFs and the
MtlManager service. For them, see the [Run Guide](../doc/getting_started/run.md).

## The scripts

### Set up and build

| Script | What it does | Root |
| --- | --- | --- |
| `mtl_install.sh` | Runs the scripts below in the correct order. Environment variables or `mtl_install_template.env` select the steps. | `sudo` for each install |
| `mtl_install_dependencies.sh` | Installs the Ubuntu packages of the MTL build, and the packages of each component that you give: `ice`, `ebpf_xdp`, `gpu_direct`, `ffmpeg`, `jpegxs`, `avcodec`, `gstreamer`, `obs`, `python`, `rust`, `readpcap`. | `sudo` |
| `mtl_build_dpdk.sh` | Downloads DPDK of `versions.env`, applies the MTL patches of `patches/dpdk/`, and builds and installs it. Does nothing if that version is installed; `-f` builds it again. | `sudo`, unless `MTL_INSTALL_PREFIX` |
| `mtl_build_drivers.sh` | Builds and installs the out-of-tree `ice` (E810, E830) and `igc` (I225, I226) kernel drivers. See the [E800 Series Driver Guide](../doc/getting_started/e800_series_drivers.md). | `sudo` |
| `mtl_build_ebpf_xdp.sh` | Checks the host, then builds and installs xdp-tools and libbpf, for the AF_XDP backend. Run it before `build.sh`. | `sudo` |
| `mtl_build_app.sh` | Builds the sample applications of `app/` into `build/app`. | no |
| `mtl_build_jpegxs.sh` | Builds and installs SVT JPEG XS and its ST 2110-22 plugin, and adds the plugin to `kahawai.json`. | `sudo`, unless `MTL_INSTALL_PREFIX` |
| `mtl_build_st22_avcodec_plugin.sh` | Builds and installs the ST 2110-22 avcodec plugin of `plugins/st22_avcodec`. | `sudo`, unless `MTL_PLUGIN_PREFIX` |
| `mtl_build_ffmpeg_plugin.sh` | Builds and installs FFmpeg with the MTL input and output devices. | `sudo`, unless `MTL_INSTALL_PREFIX` |
| `mtl_build_gstreamer_plugin.sh` | Builds and installs the MTL GStreamer plugins. | `sudo`, unless `MTL_INSTALL_PREFIX` |
| `mtl_build_obs_plugin.sh` | Builds and installs the OBS Studio plugin. | `sudo` |
| `mtl_build_python.sh` | Builds and installs the SWIG Python binding into the system `python3`. | `sudo` |

Build and install MTL itself with `./build.sh` in the top folder, after DPDK.
Build a plugin or a binding after MTL.

### Host tools

`meson install` of MTL also installs these scripts: into
`<prefix>/share/mtl/script`, with a link of the same name in `<prefix>/bin`.
So you can run them on a host that has MTL but not its source tree.

| Script | What it does | Root |
| --- | --- | --- |
| `mtl_nicctl.sh` | Binds a NIC port to the DPDK PMD or to the kernel driver, creates and removes VFs, and lists the NIC ports. The main tool to prepare a NIC for MTL. | for each change |
| `mtl_dma_setup.sh` | Binds the Intel DMA devices (CBDMA, DSA) to `vfio-pci`, and finds the DMA devices on the NUMA node of a NIC port. | for `bind` and `reset` |
| `mtl_status_report.sh` | Collects the kernel log and the system facts of the host into one `.tar` file, to attach to an issue report. | `sudo` for some commands |
| `mtl_isolation_report.sh` | Audits the isolated CPU cores with `bpftrace` and reports each task on them as EXPECTED, ALLOWED or INTRUDER. `isolation_expected.env` and `isolation_allowed.env` hold the patterns. | `sudo` for `bpftrace` |
| `mtl_isolation_monitor.sh` | Runs `mtl_isolation_report.sh` in a loop, keeps each report, and shows when the worst violations occurred. | `sudo` for `bpftrace` |
| `mtl_lcore_stat_dump.sh` | Records the scheduler switch and IRQ trace events of one CPU with ftrace. | yes |
| `mtl_if_filter_delete.sh` | Deletes all ethtool n-tuple filters of a network interface. | yes |

### Repository and CI helpers

| Script | What it does |
| --- | --- |
| `mtl_check_dpdk_patches.sh` | Checks that each patch of `patches/dpdk/` applies to its DPDK release with no offset and no fuzz. |
| `mtl_hash_sources.sh` | Computes the source checksums of the CI cache keys. `hash_sources_*.env` give the paths of each component. |
| `mtl_dos2unix.sh` | Converts the line endings of the text files that git tracks from CRLF to LF. |
| `mtl_common.sh` | The shared functions (logs, `as_root`, checks of commands and packages). Each script sources it; do not run it. |

## Typical flows

A new host with an E810 or E830 NIC, everything into the system:

```bash
SETUP_ENVIRONMENT=1 SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1 \
  SETUP_BUILD_AND_INSTALL_DPDK=1 MTL_BUILD_AND_INSTALL=1 ./script/mtl_install.sh
sudo ldconfig
```

The same, step by step:

```bash
./script/mtl_install_dependencies.sh ice
./script/mtl_build_drivers.sh --driver ice
./script/mtl_build_dpdk.sh
./build.sh
sudo ldconfig
```

Prepare a NIC port for MTL, after each boot (see the [Run Guide](../doc/getting_started/run.md)):

```bash
mtl_nicctl.sh list all                  # find the PCI address of the port
sudo mtl_nicctl.sh create_vf 0000:af:00.0   # make the VFs and bind them to vfio-pci
```

Collect the facts of the host for an issue report:

```bash
mtl_status_report.sh
```

## Add a script

Follow the rules of
[mtl-scripts.instructions.md](../.github/instructions/mtl-scripts.instructions.md):
the name starts with `mtl_`, the script sources `mtl_common.sh`, and it has a
`show_help()`, a `main()` and the sourced guard.
`.github/scripts/ci/lint/test-scripts.sh` checks each script of this folder,
and `.github/path_filters.yml` must name it.
