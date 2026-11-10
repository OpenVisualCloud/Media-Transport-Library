# Windows workflow: MTL in a Windows VM with E810 and E830 VFs

This guide takes a Linux KVM host with an Intel E810 or E830 adapter to an
ST 2110 stream of RxTxApp in a Windows guest. The guest uses SR-IOV VFs of the
adapter, the DPDK `iavf` PMD on the NetUIO driver, and rate limit pacing.

Parts 3 to 9 also apply to a Windows host on bare metal with the PF on NetUIO.
Part 2 is only for the VM mode.

The [build guide](build_WIN.md), the [run guide](run_WIN.md) and the
[VM guide](vm_WIN.md) give the short form of some of these steps.

## 1. The tested combination

| Part | Version |
| --- | --- |
| Host | Proxmox VE 9.2, Linux 7.0.2, KVM with VFIO |
| PF driver on the host | Out-of-tree `ice` 2.7.6 (`ICE_VER` of `versions.env`), DDP package 1.3.59.0 |
| Adapters | E810-C for QSFP (8086:1592), E830-CC for QSFP (8086:12d2). Each VF is 8086:1889. |
| Guest | Windows 11 Pro, build 26300, OVMF, TPM, q35 |
| Toolchain | MSYS2 UCRT64, MinGW GCC 16.2, meson 1.12 |
| DPDK | 26.07 with `patches/dpdk/26.07/*.patch` and `patches/dpdk/26.07/windows/*.patch` |
| Kernel drivers in the guest | NetUIO and virt2phys of `dpdk-kmods`, test-signed, built with the EWDK 10.0.28000 |

Results with this combination: one 1080p59 ST 2110-20 stream (2.6 Gb/s), from
one VF to the other VF of the same PF, in one RxTxApp process:

| Run | Result |
| --- | --- |
| TSC pacing, 10 min | 36041 of 36041 frames |
| Rate limit pacing, flow rules, schedulers on reserved cores, 30 s, E810 | 1858 of 1858 frames |
| The same on E830 | 1858 frames sent, 1859 received |

## 2. The Linux host

### 2.1. Firmware and kernel

1. In the BIOS, turn on VT-d and SR-IOV.

1. Turn on the IOMMU on the kernel command line:

    ```bash
    intel_iommu=on iommu=pt
    ```

1. Make sure that each VF gets its own IOMMU group:

    ```bash
    ls /sys/bus/pci/devices/<vf>/iommu_group
    ```

### 2.2. The out-of-tree ice driver

Use the out-of-tree `ice` driver of `versions.env` on the host. The in-tree
driver of the kernel does not give a VF the functions that MTL uses:

| Function | In-tree `ice` (Linux 7.0.2) | Out-of-tree `ice` 2.7.6 |
| --- | --- | --- |
| Queue rate limit (RL pacing) | `VIRTCHNL_OP_CONFIG_TC_MAP` fails | Works |
| Flow director rules of the VF | `Init FDIR for VF failed, ret:-12` | Works |
| PTP of the VF | Works | Works |

1. Install the kernel headers and the build tools. On Proxmox VE:

    ```bash
    apt install proxmox-headers-$(uname -r) build-essential
    ```

1. Build and install the driver as root. `script/build_drivers.sh --driver ice`
   of this repository does it. By hand:

    ```bash
    . versions.env
    wget https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz
    tar xzf ice-${ICE_VER}.tar.gz
    make -C ice-${ICE_VER}/src install
    ```

    `make install` puts the module in `/lib/modules/$(uname -r)/updates/`,
    and the DDP package in `/lib/firmware/updates/intel/ice/ddp/`. `modprobe`
    then takes this module at boot.

1. If the host has the in-tree `irdma` module, block it. It does not match the
   out-of-tree `ice`:

    ```bash
    printf 'blacklist irdma\ninstall irdma /bin/false\n' > /etc/modprobe.d/no-irdma.conf
    ```

1. Load the driver. Stop each VM that uses a VF of the adapter first, and set
   `sriov_numvfs` of each PF to 0:

    ```bash
    modprobe -r irdma; modprobe -r ice; modprobe ice
    cat /sys/module/ice/version
    ```

1. Check that each PF loaded the DDP package:

    ```bash
    dmesg | grep -i "DDP package"
    ```

A new host kernel boots with the in-tree driver again. Build the driver again
for each new kernel.

### 2.3. The VFs

1. Make two VFs on a PF. Give each VF a fixed MAC address, turn on the trust
   mode, and turn off the spoof check:

    ```bash
    pf=0000:c9:00.0
    inf=$(ls /sys/bus/pci/devices/$pf/net)
    ip link set "$inf" up
    echo 2 > /sys/bus/pci/devices/$pf/sriov_numvfs
    for i in 0 1; do
        ip link set "$inf" vf "$i" mac "02:00:c9:00:00:0$i" trust on spoofchk off
    done
    ```

1. The VFs do not stay after a restart of the host. Make them in a systemd
   unit that runs before the VMs start (`Before=pve-guests.service` on
   Proxmox VE, `Before=libvirtd.service` with libvirt), with `Type=oneshot`.
   Do not run it while a VM uses the VFs: a MAC change resets the VF.

1. A VF to VF stream of the same PF goes through the switch of the adapter. It
   needs no cable. A stream between two PFs needs a link between the ports.
   A VF stream also goes out of the PF port. If two hosts or VMs send the same
   multicast group and UDP port in one L2 network, each receiver gets both
   streams.

### 2.4. The VM

1. Use the q35 machine type and OVMF (UEFI).

1. Give the VM the two VFs as PCIe devices. For Proxmox VE:

    ```bash
    qm set <vmid> --hostpci0 0000:c9:01.0,pcie=1 --hostpci1 0000:c9:01.1,pcie=1
    ```

1. Use the CPU type `host`, so that the guest has AVX2 and AVX-512.

1. Put the VM on the NUMA node of the adapter. Pin the vCPUs to physical cores
   of that node, and bind the memory to it. Find the node in
   `/sys/bus/pci/devices/<pf>/numa_node`. For Proxmox VE, with the adapter on
   node 2 and CPUs 64-79 on that node:

    ```bash
    qm set <vmid> --cpu host --cores 16 --numa 1 \
        --numa0 cpus=0-15,hostnodes=2,memory=32768,policy=bind \
        --affinity 64-79 --balloon 0
    ```

1. Turn off Secure Boot. The NetUIO and virt2phys drivers are test-signed, and
   Windows loads a test-signed driver only with Secure Boot off. Use EFI
   variables with no enrolled keys:

    ```bash
    qm set <vmid> --efidisk0 local-lvm:1,efitype=4m,pre-enrolled-keys=0
    ```

    Before you do this, turn off the BitLocker protection
    (`manage-bde -protectors -disable C:`), or have the recovery key. A change
    of the Secure Boot state changes the TPM measurements.

## 3. The Windows host settings

Do these steps as an administrator. `doc/windows_setup.ps1` does all of them
except the BitLocker step. It records each value before it changes it, so you
can run it again, and `undo` puts each value back.

The MTL tree comes in part 5, so get the script alone first. Windows does not
run a script with the default execution policy (`Restricted`), so start it
with `-ExecutionPolicy Bypass`:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/OpenVisualCloud/Media-Transport-Library/main/doc/windows_setup.ps1 -OutFile windows_setup.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows_setup.ps1 status
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows_setup.ps1 apply -SchedulerCpus 8-15
powershell -NoProfile -ExecutionPolicy Bypass -File .\windows_setup.ps1 undo
```

Restart Windows after `apply`. The items:

| Item | What it sets | Why |
| --- | --- | --- |
| `testsign` | `bcdedit /set testsigning on` | NetUIO and virt2phys are test-signed. |
| `lockpages` | "Lock pages in memory" for the user | DPDK uses large pages. It takes effect at the next logon. |
| `dirs` | `C:\temp`, `C:\dpdk\lib` | The MTL lcore lockfile, and `ice.pkg` for a PF. |
| `cpusets` | `ReservedCpuSets` = `-SchedulerCpus` | Windows does not put its own threads on these CPUs. |
| `irq` | The interrupt affinity of each PCI device: the other CPUs | The devices do not interrupt the scheduler cores. |
| `power` | Ultimate Performance (or High performance), no core parking, minimum processor state 100 % | The CPUs do not slow down or park. |
| `defender` | Exclusions for `C:\mtl`, `C:\msys64`, `C:\dpdk` | The scan does not slow down the build and the run. |
| `vbs` | Virtualization-based security and Memory Integrity off | The hypervisor of VBS adds noise. |

Rules for `-SchedulerCpus`:

- Do not reserve CPU 0. Keep two CPUs or more for Windows.
- With SMT, reserve both threads of a core.
- The numbers are the CPU numbers of DPDK: the active processors, group by
  group.

A reserved CPU runs only a thread with a hard affinity mask to it
(`SetThreadGroupAffinity`). Windows ignores a CPU set selection
(`SetThreadSelectedCpuSets`) of a reserved CPU. MTL pins each scheduler thread
to its lcore on Windows, so give MTL lcores from the reserved CPUs (part 8).

Do not run MTL in `REALTIME_PRIORITY_CLASS`. The class raises each thread of
the process, also the threads with no affinity, and these threads then starve
other threads.

## 4. The build environment

1. Install MSYS2 in `C:\msys64`. The base archive needs no window. Windows
   PowerShell downloads slowly while it shows a progress bar, so turn the bar
   off:

    ```powershell
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe -OutFile msys2.sfx.exe
    .\msys2.sfx.exe -y -oC:\
    ```

1. Open a UCRT64 shell (`C:\msys64\ucrt64.exe`). Do all the next steps of
   parts 4 to 6 and 8 in that shell.

    The first start makes the pacman keyring, and then refreshes the keys from
    a key server (`pacman-key --refresh-keys`). With no direct connection to the
    internet, the refresh waits for minutes. Push Ctrl+C: the `msys2-keyring`
    package already holds the keys.

1. If the network needs a proxy, set it for MSYS2. Windows can find a proxy
   with WPAD, but curl, pacman and git of MSYS2 do not use WPAD:

    ```bash
    cat > /etc/profile.d/proxy.sh <<'EOF'
    export http_proxy=http://<proxy>:<port>
    export https_proxy=$http_proxy no_proxy=localhost,127.0.0.1
    EOF
    . /etc/profile.d/proxy.sh
    ```

1. Update MSYS2. The first update can replace the core packages and close the
   shell. Open the shell again and run the update again:

    ```bash
    pacman -Syuu --noconfirm
    ```

1. Install the packages:

    ```bash
    pacman -S --needed git pactoys unzip rsync
    pacboy -S --needed dlfcn:p gcc:p gtest:p json-c:p libpcap:p meson:p \
        mman-win32:p ninja:p pkgconf:p python-pyelftools:p
    ```

1. Install the npcap SDK:

    ```bash
    curl -LO https://npcap.com/dist/npcap-sdk-1.16.zip
    unzip -d npcap-sdk-1.16 npcap-sdk-1.16.zip
    cp -r npcap-sdk-1.16/lib/x64/. "$MSYSTEM_PREFIX/lib"
    ```

## 5. DPDK

1. Get MTL and DPDK. MTL reads the DPDK source in `<MTL>/dpdk` (the meson
   option `dpdk_root_dir`):

    ```bash
    mkdir -p /c/mtl && cd /c/mtl
    git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
    cd Media-Transport-Library
    MTL_PATH="$PWD"
    . ./versions.env
    git clone -b "v${DPDK_VER}" https://github.com/DPDK/dpdk.git
    ```

    NTFS does not tell upper case from lower case, so the MTL clone warns that
    `tests/acceptance/mtl_engine/RxTxApp.py` and `rxtxapp.py` collide. The
    Windows build does not use them.

1. Apply the patches. The patches of `windows/` are in mail format, so
   `git am` takes them too. `git am` makes commits, so git needs a name and an
   email address first:

    ```bash
    git config --global user.name "<name>"
    git config --global user.email "<email>"
    cd dpdk
    git am "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/*.patch
    git am "$MTL_PATH"/patches/dpdk/"${DPDK_VER}"/windows/*.patch
    ```

    The Windows patches of DPDK 26.07:

    | Patch | Fault that it removes |
    | --- | --- |
    | 0001 | The MinGW build, the DDP package load from `C:\dpdk\lib\ice.pkg`, the timer resolution, and no thread affinity in the EAL |
    | 0002 | MinGW GCC 15 and later use native TLS, so a DPDK DLL cannot read `rte_errno` of another DLL (`undefined reference to per_lcore__rte_errno`). DPDK builds the static libraries only. |
    | 0003 | `meson install` writes over `sched.h` of MinGW (`'RTE_MAX_LCORE' undeclared`). |
    | 0004 | `rte_eal_alarm_cancel()` frees list entries inside `LIST_FOREACH()`. A failed VF probe stops the process with a segmentation fault. |
    | 0005 | The `iavf` alarm does not read the admin queue when the PF gives `WB_ON_ITR`, and Windows has no interrupt callback. Each VF command after the init fails with `No response for cmd`. |

1. Build and install DPDK:

    ```bash
    meson setup -Dmax_lcores=256 build
    meson compile -C build
    meson install -C build
    ```

## 6. MTL

1. Build and install MTL:

    ```bash
    cd "$MTL_PATH"
    ./build.sh release
    ```

    `build.sh` installs `libmtl.dll` into the UCRT64 prefix, and builds
    `tests/tools/RxTxApp/build/RxTxApp.exe`.

1. `libmtl.dll` imports `wpcap.dll`, the runtime library of npcap. Install the
   npcap runtime, or, for a host that does not capture packets, copy the
   `libpcap.dll` of MSYS2:

    ```bash
    cp "$MSYSTEM_PREFIX/bin/libpcap.dll" "$MSYSTEM_PREFIX/bin/wpcap.dll"
    ```

1. Optional: run the unit tier. It needs no NIC:

    ```bash
    ./build.sh unit
    ```

## 7. The NetUIO and virt2phys drivers

1. Get the Enterprise WDK (EWDK) ISO from the Microsoft page "Download the
   Windows Driver Kit". Mount it, for example as `E:`.

1. Get the driver source:

    ```bash
    git clone https://dpdk.org/git/dpdk-kmods
    ```

1. The InfVerif tool of the WDK 10.0.28000 needs `PnpLockdown=1` in the
   `[Version]` section of `netuio.inf` (error 1324). Add the line:

    ```bash
    sed -i '/^\[Version\]/a PnpLockdown=1' dpdk-kmods/windows/netuio/netuio.inf
    ```

1. Build the two drivers in a command prompt (`cmd.exe`):

    ```bat
    call E:\BuildEnv\SetupBuildEnv.cmd amd64
    cd /d <path>\dpdk-kmods\windows\virt2phys
    msbuild virt2phys.sln /p:Configuration=Release /p:Platform=x64
    cd /d <path>\dpdk-kmods\windows\netuio
    msbuild netuio.sln /p:Configuration=Release /p:Platform=x64
    ```

    The build signs each driver with a test certificate (`virt2phys.cer`,
    `netuio.cer` in `x64\Release`).

1. Trust the two certificates. Then the install does not show a window:

    ```bat
    certutil -f -addstore Root x64\Release\netuio\netuio.cer
    certutil -f -addstore TrustedPublisher x64\Release\netuio\netuio.cer
    ```

    Do the same for `virt2phys.cer`. Then make sure that both stores have the
    certificate. If `TrustedPublisher` does not have it, the install fails with
    error `0xe0000242`:

    ```powershell
    Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher | Where-Object Subject -like '*WDKTestCert*'
    ```

1. Install the drivers with `devcon.exe` of the EWDK
   (`E:\Program Files\Windows Kits\10\Tools\<version>\x64\devcon.exe`).
   virt2phys is a root device. NetUIO goes on the iavf VF (`DEV_1889`); for a
   PF, use the device ID of the PF:

    ```bat
    devcon install x64\Release\virt2phys\virt2phys.inf Root\virt2phys
    devcon update x64\Release\netuio\netuio\netuio.inf "PCI\VEN_8086&DEV_1889"
    ```

    `devcon install` makes the root device first. If the driver install then
    fails, do not run `devcon install` again, because it makes a second device.
    Run `devcon update x64\Release\virt2phys\virt2phys.inf Root\virt2phys`.

1. Check that each device is `OK`:

    ```powershell
    Get-PnpDevice -PresentOnly | Where-Object { $_.Class -in 'Windows UIO','Kernel bypass' }
    sc.exe query netuio; sc.exe query virt2phys
    ```

1. Find the PCI address that DPDK uses for each VF. Windows gives "PCI bus 1,
   device 0, function 0", which is `0000:01:00.0`:

    ```powershell
    Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_8086&DEV_1889*' |
        ForEach-Object { (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_LocationInfo).Data }
    ```

## 8. RxTxApp

Windows has no MtlManager and no `--file-prefix`, so only one MTL process can
run on a host. Put the TX and the RX session in one configuration, with two
interfaces:

```json
{
    "interfaces": [
        { "name": "0000:01:00.0", "ip": "192.168.96.10" },
        { "name": "0000:02:00.0", "ip": "192.168.96.20" }
    ],
    "tx_sessions": [
        {
            "dip": [ "239.96.1.1" ],
            "interface": [ 0 ],
            "video": [
                {
                    "replicas": 1, "type": "frame", "pacing": "gap", "packing": "BPM",
                    "start_port": 20000, "payload_type": 112, "tr_offset": "default",
                    "video_format": "i1080p59", "pg_format": "YUV_422_10bit",
                    "video_url": "C:/mtl/rxtx/test_1080p.yuv"
                }
            ]
        }
    ],
    "rx_sessions": [
        {
            "ip": [ "239.96.1.1" ],
            "interface": [ 1 ],
            "video": [
                {
                    "replicas": 1, "type": "frame", "pacing": "gap",
                    "start_port": 20000, "payload_type": 112, "tr_offset": "default",
                    "video_format": "i1080p59", "pg_format": "YUV_422_10bit", "display": false
                }
            ]
        }
    ]
}
```

1. Make a source file. One 1080p frame of `YUV_422_10bit` is 5184000 bytes:

    ```bash
    mkdir -p /c/mtl/rxtx
    head -c $((1920*1080*5/2*10)) /dev/urandom > /c/mtl/rxtx/test_1080p.yuv
    ```

1. Run RxTxApp. Give MTL lcores from the reserved CPUs:

    ```bash
    ./tests/tools/RxTxApp/build/RxTxApp.exe --config_file loop_1v.json \
        --test_time 30 --pacing_way rl --lcores 8-11
    ```

1. Check the log:

    | Line | It shows |
    | --- | --- |
    | `sch_tasklet_lcore(0), pinned to lcore 8 cpu 8` | The scheduler thread is on a reserved CPU. |
    | `tv_attach(0), ... pacing way: ratelimit` | The VF accepted the queue rate limit. |
    | `rte_rx_flow_create(1), queue 1 succ` | The VF accepted the flow rule. |
    | `app_tx_video_result(0), OK` and `app_rx_video_result(0), OK` | The frame counts of TX and RX. |

1. While RxTxApp runs, each scheduler CPU is at 100 %, and the other reserved
   CPUs are idle:

    ```powershell
    (Get-Counter '\Processor(*)\% Processor Time').CounterSamples | Select-Object InstanceName, CookedValue
    ```

If the host has the in-tree `ice` driver, use `--pacing_way tsc` and
`--rss_mode l3_l4`. Then MTL needs no queue rate limit and no flow rule.

## 9. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `RxTxApp.exe: error while loading shared libraries: libpcap.dll` | `libmtl.dll` needs `wpcap.dll`, and the loader names the wrong DLL. | Install the npcap runtime, or copy `libpcap.dll` to `wpcap.dll` (part 6). |
| `iavf_wait_for_msg(): No response for cmd 60`, then each command fails | DPDK 26.07 with no patch 0005. | Apply `windows/0005`. |
| Segmentation fault in `rte_eal_alarm_cancel()` after a failed VF probe | DPDK with no patch 0004. | Apply `windows/0004`. |
| `undefined reference to per_lcore__rte_errno` when DPDK builds | MinGW GCC with native TLS and no patch 0002. | Apply `windows/0002`. |
| `iavf_set_q_tc_map(): Failed to execute command of VIRTCHNL_OP_CONFIG_TC_MAP` | The in-tree `ice` on the host. | Use the out-of-tree `ice` (part 2.2), or `--pacing_way tsc`. |
| `Init FDIR for VF 1 failed, ret:-12` on the host, `rte_flow_create fail` in MTL | The in-tree `ice` on the host. | Use the out-of-tree `ice`, or `--rss_mode l3_l4`. |
| `arp_get_result(0), cache fail as timeout` | A unicast stream between two ports of one process. | Use a multicast destination. |
| Millions of `pkts dropped: tmstamp outside OFO window` in 10 s, and each frame arrives | A second sender uses the same multicast group and UDP port in the same L2 network. A VF stream also goes out of the PF to the switch, and a switch with no IGMP snooping sends it to each port. | Give each sender its own group or port. |
| `cannot set log level user,8` with `--log_level debug` | MTL before the fix of the debug log level. | Update MTL. |
| `devcon.exe failed`, and `C:\Windows\INF\setupapi.dev.log` has `Error 0xe0000242: The publisher of an Authenticode(tm) signed catalog has not yet been established as trusted` | The test certificate is not in `TrustedPublisher`. | Add it (part 7), then run `devcon update`. |
| `bcdedit` refuses `testsigning` | Secure Boot is on. | Turn Secure Boot off (part 2.4). |
| A scheduler thread is not on a reserved CPU | `--lcores` names CPUs that are not reserved, or MTL before the scheduler pinning. | Use reserved CPUs in `--lcores`, and update MTL. |
| `failed retrieving file ... from mirror.msys2.org : SSL certificate ... unable to get local issuer certificate` | The certificate chain of that one mirror. | Nothing: pacman takes the file from the next mirror. |
| `Warn: st_plugin_register, dlopen /usr/local/lib/...so fail` | RxTxApp reads `kahawai.json` of the current folder, and the plugin paths of the MTL tree are for Linux. | Nothing: it is a warning. Start RxTxApp in another folder to remove it. |
| A UAC prompt has the focus on No | This is the default of Windows. | Click Yes. Enter selects No. |

## 10. Limits

- NetUIO and virt2phys are test-signed. Windows loads them only with Secure
  Boot off and test signing on, so this setup is for a lab.
- One MTL process for each host.
- The clock tick still comes to each reserved CPU. In a VM, each reserved CPU
  had about 1000 gaps of 5 µs or more each second, and 2 to 10 gaps of 13.5 µs
  or more each second. A number for a product needs bare metal.
- MTL on Windows adjusts the system time only in steps. The PTP servo does not
  adjust the frequency.
