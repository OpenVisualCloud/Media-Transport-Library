# Run guide for Windows

Install MTL with the installer, then test it. To build MTL from the
source, see the [build guide](build_WIN.md). For a Windows VM with VFs, see the
[VM guide](vm_WIN.md).

## 1. Install MTL

1. Turn off Secure Boot in the firmware: the drivers are test-signed. If
   BitLocker is on, suspend it first: `manage-bde -protectors -disable C:`.

1. Download the Enterprise WDK (EWDK) ISO from the Microsoft page "Download
   the Windows Driver Kit" to your Downloads folder. Setup finds it there and
   builds the drivers with it.

1. Get the installer `mtl-<version>-win64-setup.exe`. On GitHub, open
   **Actions > Windows MSYS2 build**, select the newest run of `main`, and
   download the `mtl-windows-installer` artifact (sign in to GitHub first).
   Or make the installer of your own build: part 4 of the
   [build guide](build_WIN.md).

1. Run the installer. On the "Host settings" page:

    - **Scheduler CPUs**: keep `auto` (the last half of the cores, at most 8
      CPUs), or give a list, for example `8-15`.
    - **NetUIO device IDs**: nothing for a VF. For a PF, its ID, for example
      `PCI\VEN_8086&DEV_1592` (E810-C for QSFP) or `PCI\VEN_8086&DEV_12D2`
      (E830-CC for QSFP). Never give the ID of the NIC that Windows uses for
      its own network.

1. Restart Windows.

Setup installs MTL in `C:\Program Files\MTL`, puts the DDP package of the
E810 and E830 in `C:\dpdk\lib\ice.pkg`, and writes its logs in
`C:\ProgramData\MTL\logs`. To remove MTL and put each Windows setting back,
uninstall MTL in Settings > Apps.

## 2. Test MTL

1. Double-click **MTL test** on the desktop, and let it make changes. It sends
   two 1080p59 video, two audio and two ancillary streams from port 0 to port
   1 for 30 s, and ends with `PASS` or `FAIL`. The two ports must reach each
   other: two VFs of one PF, or two PFs on one network.

1. To run your own streams, start from the configuration of the test,
   `C:\ProgramData\MTL\test\test.json`:

    ```powershell
    RxTxApp.exe --config_file my.json --test_time 30
    ```

**Start > MTL > MTL status** shows each Windows setting and the PCI address of
each NetUIO port.

## 3. Troubleshooting

| Symptom | Fix |
| --- | --- |
| Setup tells "Secure Boot is on" | Turn off Secure Boot, then run setup again. |
| Setup shows no driver task | No EWDK found. Put the EWDK ISO in Downloads, then open **Start > MTL > MTL install drivers**. |
| MTL test: "The test needs two NetUIO ports" | Give NetUIO the device ID of each port (run setup again), or connect the second PF. |
| "MTL status": `build` is `error` | See `C:\ProgramData\MTL\drivers\build.log`. |
| "MTL status": a port is not `OK` after the restart | Test signing is off (the `testsign` item), or Secure Boot is on. |
| `RxTxApp.exe` is not found | Open a new PowerShell window after setup. |
| `VIRTCHNL_OP_CONFIG_TC_MAP` fails, or `rte_flow_create fail` on a VF | The host uses the in-tree `ice` driver. Use the out-of-tree driver (VM guide), or run with `--pacing_way tsc --rss_mode l3_l4`. |
| `arp_get_result ... timeout` | A unicast stream between two ports of one process. Use a multicast address. |
| Millions of `pkts dropped: tmstamp outside OFO window`, but each frame arrives | A second sender uses the same multicast group and port. Give each sender its own group. |
