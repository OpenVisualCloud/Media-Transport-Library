# Troubleshooting and FAQ

This page holds the known problems of the [Build Guide](build.md), the [E800 Series Driver Guide](e800_series_drivers.md) and the [Run Guide](run.md), and the answers to frequent questions.
Find the error message that you see, then do the steps of its section.

If a problem stays, collect the status of the host and attach the `.tar` file to your issue report:

```bash
./script/mtl_status_report.sh
```

## 1. Build Problems

### 1.1. DPDK Is Not Found

It may get below error caused by PKG_CONFIG_PATH path problem.

```text
lib/meson.build:10:0: ERROR: Dependency "libdpdk" not found, tried pkgconfig
```

Try below command to find the pc path and add to the PKG_CONFIG_PATH env.

```bash
find / -name libdpdk.pc
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/:/usr/local/lib/pkgconfig/
```

### 1.2. libnuma Is Not Found

DPDK needs libnuma. Look for this line in the log of `meson setup build` of DPDK:

```text
Library numa found: YES
```

If it shows `NO`, install `libnuma-dev` (Ubuntu, Debian) or `numactl-devel` (CentOS, RHEL), then build DPDK again.

### 1.3. sudo Does Not Find ninja

If `sudo ninja install -C build` shows this error, `sudo` does not have `/usr/local/bin` in its path:

```text
sudo: ninja: command not found
```

Add it as [secure_path for the root User](build_advanced.md#36-secure_path-for-the-root-user) shows.

### 1.4. The Kernel Header Files Are Missing

If the build of a kernel driver shows this error, the header files of the running kernel are missing:

```text
*** Kernel header files not in any of the expected locations.
```

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

### 1.5. An Application Does Not Find a Shared Library

If you get a similar message when running the RxTxApp, it's likely an `ld` library path problem.

```text
./tests/tools/RxTxApp/build/RxTxApp: error while loading shared libraries: librte_dmadev.so.23: cannot open shared object file: No such file or directory
```

Try to find the path of this so and append it to `LD_LIBRARY_PATH`.

```bash
find / -name librte_dmadev.so.23
# /usr/local/lib64/librte_dmadev.so.23
```

```bash
# Note to change the path as the find result
export LD_LIBRARY_PATH=/usr/local/lib64/
# Or use ldconfig to update the cache
sudo ldconfig
```

`./script/mtl_build_dpdk.sh` and `./build.sh` install into `/usr/local`. Run `sudo ldconfig` after each of them. On CentOS Stream, RHEL and Rocky Linux, first add `/usr/local/lib64` to the search path, as [The Library Path](build_advanced.md#33-the-library-path-of-centos-stream-rhel-and-rocky-linux) shows.

### 1.6. meson Refuses the Build Directory

If `sudo ninja install` or `./build.sh` shows this error, two different meson versions read the same build directory:

```text
ERROR: Build directory has been generated with Meson version 1.2.0, which is incompatible with the current version 1.3.2.
```

This happens after `pip install --user meson`: as your user, `meson` loads the `pip` copy, but `sudo` loads the meson of the distribution. Remove the `pip` copy, and build again in a new build directory:

```bash
pip3 uninstall meson          # add --break-system-packages on Ubuntu 24.04 and later
meson --version               # now the version of the distribution
rm -rf build && ./build.sh
```

### 1.7. The Manager Build Does Not Find gnu/stubs-32.h

If `./build.sh` shows one of these errors, the host has libxdp and libbpf, but not the 32-bit libc headers:

```text
fatal error: 'gnu/stubs-32.h' file not found
ERROR: libxdp and libbpf are installed, so the manager builds its XDP program, which needs the 32-bit libc headers.
```

With libxdp and libbpf, the MtlManager builds its XDP program, and `clang -target bpf` needs these headers. A distribution DPDK package (`dpdk-dev`) also installs libxdp. Install the headers, then build again:

```bash
sudo apt-get install gcc-multilib     # Ubuntu, Debian
sudo dnf install glibc-devel.i686     # RHEL, Rocky Linux
```

### 1.8. DPDK Cannot Install into a Path with a Space

```text
ERROR: DPDK cannot install into a path with a space: /home/user/my src/.local_install/dpdk
```

Set `MTL_INSTALL_PREFIX` to a path with no space. The source tree can have a space in its path.

## 2. Driver Problems

### 2.1. modprobe ice Shows "Unknown symbol"

If the install of the driver ends with this error, the kernel does not have the `gnss` module that the driver needs:

```text
modprobe: ERROR: could not insert 'ice': Unknown symbol in module, or unknown parameter (see dmesg)
```

`sudo dmesg | tail` shows `ice: Unknown symbol gnss_register_device`. A cloud or minimal Ubuntu install (`linux-image-virtual`) has `gnss` only in the extra modules. Install them, and load the driver again:

```bash
./script/mtl_install_dependencies.sh ice        # installs linux-modules-extra-$(uname -r)
./script/mtl_build_drivers.sh --driver ice
```

### 2.2. The ice Module Is in Use by irdma

If `rmmod ice` shows this error, run `sudo rmmod irdma`, then do the command again:

```text
rmmod: ERROR: Module ice is in use by: irdma
```

The Media Transport Library does not use `irdma`, so leave it unloaded.
If something else on this host needs RDMA, run `sudo modprobe irdma`.

### 2.3. The Kernel Header Files Are Missing

See [section 1.4](#14-the-kernel-header-files-are-missing).

### 2.4. The DDP Package Is Older Than 1.3.35.0

See [Earlier E800 Drivers and DDP Packages](e800_legacy.md#2-a-ddp-package-older-than-13350).

## 3. Run Problems

### 3.1. The Port Is Not Bound to the DPDK PMD

The following error indicates that the port driver is not configured to DPDK PMD mode. Please run mtl_nicctl.sh to configure it:

```text
ST: st_dev_get_socket, failed to locate 0000:86:20.0
ST: st_dev_get_socket, please make sure the driver of 0000:86:20.0 is configured to DPDK PMD
```

### 3.2. No Hugepages

If you encounter the following hugepage error while running, it is likely caused by the absence of 1G or 2M huge pages in the current setup.

```text
EAL: FATAL: Cannot get hugepage information.
EAL: Cannot get hugepage information.
```

This error message usually indicates that the mbuf pool creation has failed due to insufficient huge pages. Please try to allocate more huge pages.

```text
ST: st_init, mbuf_pool create fail
```

### 3.3. No Access to the VFIO Device

If you encounter the following error message, do section 3 of the [Run Guide](run.md#3-give-your-user-access-to-vfio): create the group `vfio` and add your user to it:

```text
EAL: Cannot open /dev/vfio/147: Permission denied
EAL: Failed to open VFIO group 147
```

### 3.4. The Link Is Not Connected

The following error indicates that the physical port link is not connected to a network. Please confirm that the cable link is working properly.

```text
ST: dev_create_port(0), link not connected
```

### 3.5. DMA Remapping Fails for a User That Is Not root (RLIMIT_MEMLOCK)

Ubuntu sets no limit, so this happens mostly on CentOS Stream and RHEL 9.

RLIMIT_MEMLOCK (amount of pinned pages the process is allowed to have), if you see the below error at start up, it's likely caused by too small RLIMIT_MEMLOCK settings.

```text
EAL: Cannot set up DMA remapping, error 12 (Cannot allocate memory)
EAL: 0000:af:01.0 DMA remapping failed, error 12 (Cannot allocate memory)
EAL: Requested device 0000:af:01.0 cannot be used
```

Please increase the value of RLIMIT_MEMLOCK. Below is an example to enable unlimited RLIMIT_MEMLOCK.

Edit `/etc/security/limits.conf` file, append below two lines at the end of the file, change <USER> to the username currently logged in:
```text
<USER>    hard   memlock           unlimited
<USER>    soft   memlock           unlimited
```
```bash
sudo reboot
```

After reboot, double-check the limit is disabled.

```bash
ulimit -a | grep "max locked memory"
```
```text
max locked memory       (kbytes, -l) unlimited
```

### 3.6. Lower fps with ptp4l and phc2sys

You may have noticed a similar epoch drop log, which is likely caused by both NTP and phc2sys adjusting the system. To address this issue, please disable the NTP service.

```text
MT: DEV(0): Avr rate, tx: 4789 Mb/s, rx: 0 Mb/s, pkts, tx: 4525950, rx: 9
MT: PTP(0): time 1676254936223518377, 2023-02-13 10:22:16
MT: CNI(0): eth_rx_cnt 9
MT: TX_VIDEO_SESSION(0,0:app_tx_video_0): fps 27.499879, frame 275 pkts 4526532:4525984 inflight 279856:279869, cpu busy 75.286346
MT: TX_VIDEO_SESSION(0,0): dummy pkts 550, burst 550
MT: TX_VIDEO_SESSION(0,0): mismatch epoch troffset 275
MT: TX_VIDEO_SESSION(0,0): epoch drop 275
```

### 3.7. The lcore Init Fails

This might happen after commit `4f46e49`, because the lcore_shm structure is [changed](https://github.com/OpenVisualCloud/Media-Transport-Library/commit/4f46e493b79451c7ca564d82e1be56c7916b0722#diff-7ff8a138885ebda032ff57250ff81174b30722cc168a26fe39d9a4ff501d48d0L710).
It also happens when the structure of the starting process is larger than the one in the system: its size follows `RTE_MAX_LCORE` of the DPDK build, so it happens while a process of a DPDK with a smaller `RTE_MAX_LCORE`, or an MTL 26.09 or earlier, runs.
Stop the older processes first: MTL 26.09 and earlier must not run together with a later release (see the [lcore Guide](../performance/shm_lcore.md)).
`mtl_init` replaces a smaller structure that no process uses, such as one left by a killed or crashed process, if it runs as root or as the user that created it.

```text
MTL: 2026-11-04 12:11:09, Error: sch_lcore_shm_remove_unused, shared memory for lcore of 22532 bytes in use, nattch 1
MTL: 2026-11-04 12:11:09, Error: sch_init_lcores, lcore init fail -5
MTL: 2026-11-04 12:11:09, Error: dev_sch_init, sch mgr init fail -5
MTL: 2026-11-04 12:11:09, Error: mt_dev_create, dev_sch_init fail -5
MTL: 2026-11-04 12:11:09, Error: mt_main_create, mt_dev_create fail -5
MTL: 2026-11-04 12:11:09, Error: mtl_init, st main create fail -5
```

Follow the below steps to clear the old shared memory that `mtl_init` can not remove, once `ipcs -m` shows `nattch` 0 for it:

```bash
# find the shmid of the old shm
ipcs -m
# delete the old shm
sudo ipcrm -m <shmid>
```

## 4. Frequent Questions

### 4.1. How Do I Find the BDF of a Port That MTL Can Use?

```bash
dpdk-devbind.py -s
```

And check the device which are DPDK-compatible.

```text
Network devices using DPDK-compatible driver
============================================
0000:af:01.0 'Ethernet Adaptive Virtual Function 1889' drv=vfio-pci unused=iavf
0000:af:01.1 'Ethernet Adaptive Virtual Function 1889' drv=vfio-pci unused=iavf
0000:af:01.2 'Ethernet Adaptive Virtual Function 1889' drv=vfio-pci unused=iavf
0000:af:01.3 'Ethernet Adaptive Virtual Function 1889' drv=vfio-pci unused=iavf
0000:af:01.4 'Ethernet Adaptive Virtual Function 1889' drv=vfio-pci unused=iavf
0000:af:01.5 'Ethernet Adaptive Virtual Function 1889' drv=vfio-pci unused=iavf
```

### 4.2. How Do I Bind a Port Back to the Kernel Driver?

```bash
sudo ./script/mtl_nicctl.sh bind_kernel 0000:af:00.0
```

### 4.3. How Do I Bind a PF to the DPDK PMD?

```bash
sudo ./script/mtl_nicctl.sh bind_pmd 0000:af:00.0
```

### 4.4. How Do I Create Trusted VFs?

To allow privileged usage in use cases that require Trusted Virtual Functions (VFs), you can utilize the following command:

```bash
sudo ./script/mtl_nicctl.sh create_tvf 0000:af:00.0
```

If you need to control the Trusted setting on a per-VF basis, you can use the following example command:

```bash
sudo ip link set enp24s0f0 vf 0 trust on
```

### 4.5. Can VFIO Run Without an IOMMU?

The VFIO driver can run without the IOMMU feature. Enable it with the below command to bypass IOMMU. As the name suggests, `enable_unsafe_noiommu_mode` is considered unsafe and should only be used if you understand the risks.

```bash
sudo bash -c 'echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode'
```
