# Run guide for Windows

Do the [build guide](build_WIN.md) first. For a Windows VM with VFs, also see
the [VM guide](vm_WIN.md).

## 1. Prepare Windows (one time)

1. Turn off Secure Boot in the firmware. The NetUIO and virt2phys drivers are
   test-signed, and Windows loads them only with Secure Boot off. If BitLocker
   is on, suspend it first: `manage-bde -protectors -disable C:`.

1. In PowerShell as administrator, run the setup script and restart:

    ```powershell
    cd C:\mtl\Media-Transport-Library\doc
    powershell -NoProfile -ExecutionPolicy Bypass -File .\windows_setup.ps1 apply -SchedulerCpus 8-15
    Restart-Computer
    ```

    The script sets test signing, "Lock pages in memory" (large pages),
    `C:\temp`, the reserved CPUs for MTL, the interrupts on the other CPUs,
    the power plan, Defender exclusions and VBS off. `status` shows each
    item, and `undo` puts each item back.

    For `-SchedulerCpus`: do not use CPU 0, keep two CPUs or more for Windows,
    and with SMT reserve both threads of a core.

## 2. Install the NetUIO and virt2phys drivers (one time)

1. Get the Enterprise WDK (EWDK) ISO from the Microsoft page "Download the
   Windows Driver Kit", and mount it (here `E:`).

1. In the UCRT64 shell, get the driver source:

    ```bash
    cd /c/mtl && git clone https://dpdk.org/git/dpdk-kmods
    sed -i '/^\[Version\]/a PnpLockdown=1' dpdk-kmods/windows/netuio/netuio.inf
    ```

    The WDK 10.0.28000 needs the `PnpLockdown=1` line.

1. Build the drivers in a command prompt (`cmd.exe`):

    ```bat
    call E:\BuildEnv\SetupBuildEnv.cmd amd64
    cd /d C:\mtl\dpdk-kmods\windows\virt2phys
    msbuild virt2phys.sln /p:Configuration=Release /p:Platform=x64
    cd /d C:\mtl\dpdk-kmods\windows\netuio
    msbuild netuio.sln /p:Configuration=Release /p:Platform=x64
    ```

1. Install the drivers, in PowerShell as administrator. The build signs them
   with a test certificate: trust it, then install with `devcon` of the EWDK:

    ```powershell
    cd C:\mtl\dpdk-kmods\windows
    foreach ($cer in 'virt2phys\x64\Release\virt2phys.cer', 'netuio\x64\Release\netuio\netuio.cer') {
        certutil -f -addstore Root $cer
        certutil -f -addstore TrustedPublisher $cer
    }
    $devcon = (Get-ChildItem 'E:\Program Files\Windows Kits\10\Tools\*\x64\devcon.exe')[0].FullName
    & $devcon install virt2phys\x64\Release\virt2phys\virt2phys.inf Root\virt2phys
    & $devcon update netuio\x64\Release\netuio\netuio\netuio.inf "PCI\VEN_8086&DEV_1889"
    ```

    `DEV_1889` is the iavf VF. For a PF, use the device ID of the PF (for
    example `DEV_1592` for the E810-C for QSFP), and copy `ice.pkg` of the
    [Linux ice driver package](https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-800-series-devices-under-linux.html)
    to `C:\dpdk\lib\ice.pkg`.

1. Check that each device is `OK`:

    ```powershell
    Get-PnpDevice -PresentOnly | Where-Object { $_.Class -in 'Windows UIO', 'Kernel bypass' }
    ```

## 3. Run RxTxApp

Only one MTL process can run on a Windows host. Put the TX and the RX session
in one configuration file.

1. Find the PCI address of each port. Windows gives "PCI bus 1, device 0,
   function 0", which is `0000:01:00.0`:

    ```powershell
    Get-PnpDevice -PresentOnly -Class 'Windows UIO' |
        ForEach-Object { (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_LocationInfo).Data }
    ```

1. Make a configuration file, for example `loop.json`: one 1080p59 stream from
   port 0 to port 1.

    ```json
    {
        "interfaces": [
            { "name": "0000:01:00.0", "ip": "192.168.96.10" },
            { "name": "0000:02:00.0", "ip": "192.168.96.20" }
        ],
        "tx_sessions": [ { "dip": [ "239.96.1.1" ], "interface": [ 0 ],
            "video": [ { "replicas": 1, "type": "frame", "pacing": "gap", "packing": "BPM",
                "start_port": 20000, "payload_type": 112, "tr_offset": "default",
                "video_format": "i1080p59", "pg_format": "YUV_422_10bit",
                "video_url": "C:/mtl/test_1080p.yuv" } ] } ],
        "rx_sessions": [ { "ip": [ "239.96.1.1" ], "interface": [ 1 ],
            "video": [ { "replicas": 1, "type": "frame", "pacing": "gap",
                "start_port": 20000, "payload_type": 112, "tr_offset": "default",
                "video_format": "i1080p59", "pg_format": "YUV_422_10bit", "display": false } ] } ]
    }
    ```

1. Make a source file and run RxTxApp in the UCRT64 shell. Give MTL lcores of
   the reserved CPUs:

    ```bash
    head -c $((1920*1080*5/2*10)) /dev/urandom > /c/mtl/test_1080p.yuv
    cd /c/mtl/Media-Transport-Library
    ./tests/tools/RxTxApp/build/RxTxApp.exe --config_file loop.json --test_time 30 --pacing_way rl --lcores 8-11
    ```

1. Check the log for these lines:

    | Line | It shows |
    | --- | --- |
    | `sch_tasklet_lcore(0), pinned to lcore 8 cpu 8` | The scheduler runs on a reserved CPU. |
    | `pacing way: ratelimit` | The NIC paces the stream. |
    | `rte_rx_flow_create(1), queue 1 succ` | The flow rule for RX works. |
    | `app_tx_video_result(0), OK` and `app_rx_video_result(0), OK` | The frame counts of TX and RX. |

## 4. Troubleshooting

| Symptom | Fix |
| --- | --- |
| `error while loading shared libraries: libpcap.dll` | `wpcap.dll` is missing. See part 3 of the build guide. |
| `bcdedit` refuses test signing | Turn off Secure Boot. |
| `devcon.exe failed`, error `0xe0000242` in `C:\Windows\INF\setupapi.dev.log` | The test certificate is not in `TrustedPublisher`. Add it, then run `devcon update` (not `install` again: that makes a second device). |
| `No response for cmd`, or a crash in `rte_eal_alarm_cancel()` | DPDK without the Windows patches 0004 and 0005. Build DPDK again. |
| `VIRTCHNL_OP_CONFIG_TC_MAP` fails, or `rte_flow_create fail` on a VF | The host uses the in-tree `ice` driver. Use the out-of-tree driver (VM guide), or run with `--pacing_way tsc --rss_mode l3_l4`. |
| `arp_get_result ... timeout` | A unicast stream between two ports of one process. Use a multicast address. |
| Millions of `pkts dropped: tmstamp outside OFO window`, but each frame arrives | A second sender uses the same multicast group and port. Give each sender its own group. |
| `Warn: st_plugin_register, dlopen /usr/local/lib/...` | Only a warning: `kahawai.json` of the current folder lists Linux plugins. |
| `failed retrieving file ... mirror.msys2.org ... local issuer certificate` | Only a warning: pacman uses the next mirror. |
