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

1. Double-click **MTL test** on the desktop, let it make changes, and give the
   number of sets and the test time (`30`, `20m`, `1h`). Each set is two
   1080p59 video, two audio and two ancillary streams, from port 0 to port 1.
   A key stops the test early. The test ends with `PASS` or `FAIL`. The two
   ports must reach each other: two VFs of one PF, or two PFs on one network.
   A VF takes 2 sets, or more with a large VF (VM guide).

1. To run your own streams, start from the configuration of the test,
   `C:\ProgramData\MTL\test\test.json`:

    ```powershell
    RxTxApp.exe --config_file my.json --test_time 30
    ```

**Start > MTL > MTL status** shows each Windows setting and the PCI address of
each NetUIO port.

## 3. Performance

Measured on two E810 VFs in a Windows 11 VM: 10 st20p TX and 10 st20p RX
sessions of 2160p29.97 YUV 4:2:2 10-bit, with a format conversion on each
side (YUV422PLANAR10LE to RFC 4175 and back), 52 Gb/s TX and 52 Gb/s RX.
Each frame arrived. In a VM, also do the host steps of the
[VM guide](vm_WIN.md).

1. **Run TX and RX in two processes, and put all the TX video sessions on
   one scheduler.** Each TX scheduler adds its own late visits, and in the
   tests the long stalls came only with TX and RX in one process.
   `--sch_session_quota N` sets the load of a scheduler in 1080p59 streams;
   give the total of the TX video streams (a 2160p29.97 stream counts 2, so
   10 of them need 20). Give each process its own CPUs of the reserved set,
   and start RX first:

    ```powershell
    RxTxApp.exe --config_file rx.json --lcores 25-31 --test_time 612
    RxTxApp.exe --config_file tx.json --lcores 24 --sch_session_quota 20 --test_time 600
    ```

    | Set-up, 10 min | `recalculate warmup` per min | Stalls of 0.08-48 ms |
    | --- | --- | --- |
    | One process, 2 TX schedulers | 77-146 | 1-3 |
    | TX and RX in two processes, 1 TX scheduler | 43-52 | none from 40 min after a start of Windows; 2 before |

    The RX process runs longer than the TX process, so its fps verdict can
    say FAILED when each frame arrived. Compare the frame counts.

1. **Plan the Windows updates.** An update of the Defender security
   intelligence stopped the VM for 22 ms during a test, and the RX port
   dropped 333290 packets; a scan of Windows Update stopped it for 45 ms.
   Setup excludes the MTL folders, `C:\dpdk` and `RxTxApp.exe` from the
   Defender scans, but it does not stop the updates. Let them run outside
   the times when MTL sends streams. `Stop-Service` is not sufficient: a
   trigger starts the services again. For a test, disable them, and enable
   them again after it:

    ```powershell
    Set-Service wuauserv, UsoSvc -StartupType Disabled; Stop-Service wuauserv, UsoSvc -Force
    # the test
    Set-Service wuauserv -StartupType Manual; Set-Service UsoSvc -StartupType Automatic
    ```

1. **Do not start a long test just after a start of Windows.** In the first
   minutes the start-up work stopped all CPUs at one time for up to 4 ms.
   With TX and RX in two processes, the 10 min tests that started 5 to 17
   min after a start of Windows had 2 stalls of 0.08 to 45 ms each; the tests
   that started 40 and 62 min after a start had none.

1. **Read the log.** `TX_VIDEO_SESSION(m,s): transmitter recalculate warmup N`
   tells that the scheduler thread came late N times in the rate limit
   warm-up of a frame, a stall of about 2 us or more. Many of these lines
   tell that the scheduler CPUs get interrupts or other work: check
   **MTL status** (`cpusets`, `irq`) and, in a VM, the host steps of the
   VM guide.

These gave no change in the measurements: the Windows dynamic tick off
(`bcdedit /set disabledynamictick yes`), and no timer resolution request of
DPDK.

## 4. Troubleshooting

| Symptom | Fix |
| --- | --- |
| Setup tells "Secure Boot is on" | Turn off Secure Boot, then run setup again. |
| Setup shows no driver task | No EWDK found. Put the EWDK ISO in Downloads, then open **Start > MTL > MTL install drivers**. |
| MTL test: "The test needs two NetUIO ports" | Give NetUIO the device ID of each port (run setup again), or connect the second PF. |
| "MTL status": `build` is `error` | See `C:\ProgramData\MTL\drivers\build.log`. |
| "MTL status": a port is not `OK` after the restart | Test signing is off (the `testsign` item), or Secure Boot is on. |
| `RxTxApp.exe` is not found | Open a new PowerShell window after setup. |
| `VIRTCHNL_OP_CONFIG_TC_MAP` fails, or `rte_flow_create fail` on a VF | The host uses the in-tree `ice` driver. Use the out-of-tree driver (VM guide), or run with `--pacing_way tsc --rss_mode l3_l4`. |
| `arp_get_result ... timeout` | A unicast stream between two ports of one process. Give the MAC of the RX port (`--p_tx_dst_mac <MAC>`), or use a multicast address. |
| Millions of `pkts dropped: tmstamp outside OFO window`, but each frame arrives | A second sender uses the same multicast group and port. Give each sender its own group. |
