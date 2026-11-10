# Run guide for Windows

Install MTL with the installer, then run RxTxApp. To build MTL from the
source, see the [build guide](build_WIN.md). For a Windows VM with VFs, see the
[VM guide](vm_WIN.md).

## 1. Install MTL

1. Turn off Secure Boot in the firmware: the drivers are test-signed. If
   BitLocker is on, suspend it first: `manage-bde -protectors -disable C:`.

1. Mount the Enterprise WDK (EWDK) ISO, from the Microsoft page "Download the
   Windows Driver Kit". Setup builds the drivers with it.

1. Run `mtl-<version>-win64-setup.exe`, the `mtl-windows-installer` artifact
   of the CI job "Windows MSYS2 build". On the "Host settings" page, give:

    - **Scheduler CPUs**: the CPUs for MTL only, for example `8-15`. Do not
      use CPU 0, keep two CPUs or more for Windows, and with SMT give both
      threads of a core.
    - **NetUIO device IDs**: nothing for a VF. For a PF, its ID, for example
      `PCI\VEN_8086&DEV_1592` (E810-C for QSFP) or `PCI\VEN_8086&DEV_12D2`
      (E830-CC for QSFP). Never give the ID of the NIC that Windows uses for
      its own network.

1. Restart Windows.

1. For a PF only: copy `ice.pkg` of the
   [Linux ice driver package](https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-800-series-devices-under-linux.html)
   to `C:\dpdk\lib\ice.pkg`.

1. Open **Start > MTL > MTL status**. Each item must be `ok`. The `netuio`
   line gives the PCI address of each port.

Setup installs MTL in `C:\Program Files\MTL`, and writes its logs in
`C:\ProgramData\MTL\logs`. To remove MTL and put each Windows setting back,
uninstall MTL in Settings > Apps.

## 2. Run RxTxApp

1. In PowerShell, copy the sample configuration and make a source file of 10
   frames:

    ```powershell
    mkdir -Force C:\mtl; cd C:\mtl
    copy 'C:\Program Files\MTL\config\loop.json' .
    $b = New-Object byte[] (1920 * 1080 * 5 / 2 * 10); (New-Object Random).NextBytes($b)
    [IO.File]::WriteAllBytes('C:\mtl\test_1080p.yuv', $b)
    ```

1. In `loop.json`, set the PCI addresses of your two ports. The file sends one
   1080p59 stream from port 0 to port 1.

1. Run RxTxApp on lcores of the scheduler CPUs:

    ```powershell
    RxTxApp.exe --config_file loop.json --test_time 30 --pacing_way rl --lcores 8-11
    ```

1. At the end, the log shows `app_tx_video_result(0), OK` and
   `app_rx_video_result(0), OK`.

## 3. Troubleshooting

| Symptom | Fix |
| --- | --- |
| Setup tells "Secure Boot is on" | Turn off Secure Boot, then run setup again. |
| Setup shows no driver task | No EWDK is mounted. Mount it, then open **Start > MTL > MTL install drivers**. |
| "MTL status": `build` is `error` | See `C:\ProgramData\MTL\drivers\build.log`. |
| "MTL status": a port is not `OK` after the restart | Test signing is off (the `testsign` item), or Secure Boot is on. |
| `RxTxApp.exe` is not found | Open a new PowerShell window after setup. |
| `VIRTCHNL_OP_CONFIG_TC_MAP` fails, or `rte_flow_create fail` on a VF | The host uses the in-tree `ice` driver. Use the out-of-tree driver (VM guide), or run with `--pacing_way tsc --rss_mode l3_l4`. |
| `arp_get_result ... timeout` | A unicast stream between two ports of one process. Use a multicast address. |
| Millions of `pkts dropped: tmstamp outside OFO window`, but each frame arrives | A second sender uses the same multicast group and port. Give each sender its own group. |
