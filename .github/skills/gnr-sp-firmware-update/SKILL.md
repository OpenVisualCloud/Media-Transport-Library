---
name: gnr-sp-firmware-update
description: 'Update BIOS, BMC, CPLD (MPestiFPTarget/MPestiInitiator), and CPU microcode on Granite Rapids-SP (GNR-SP) QuantaGrid D55Q-2U S7Q hosts running OpenBMC. Use when asked to flash, upgrade, downgrade, or verify platform firmware on a QCT S7Q / D55Q / GNR-SP server, or when troubleshooting Redfish multipart push errors (HTTP 400 ActionParameterNotSupported, ApplyTime not supported, Preserve conflict, ServiceTemporarilyUnavailable, silent updates) or activation issues (CPLD still old after reboot).'
---

# GNR-SP Platform Firmware Update (QCT D55Q-2U S7Q / OpenBMC)

## When to Use
- Flash BIOS, BMC, or CPLD on a QCT GNR-SP host via Redfish multipart push
- Update CPU microcode on Ubuntu hosts (apt-managed `intel-microcode`)
- Verify firmware versions across BMC + host
- Diagnose: `ApplyTime` 400, `Preserve` 400, HTTP 503 during silent updates, dual-bank BMC confusion, CPLD-not-activating-after-reboot

## Target Platform
- Server: QuantaGrid D55Q-2U **S7Q** (Granite Rapids-SP, e.g. Xeon 6767P sig `0x000a06d1`)
- BMC: OpenBMC (verified on 3.08 → 3.20)
- Source firmware images: <https://github.com/intel-sandbox/Media-Transport-Library-Devtools/tree/main/gnr_sp_platform>
- Verification helper: `gnr_check_firmware.sh` in same repo

## Critical Network Rule (from corp-proxy hosts)
**Always** prefix BMC `curl` calls with env vars, not `--noproxy *`:
```bash
no_proxy='*' NO_PROXY='*' curl -sk -u admin:'<pw>' https://<BMC_IP>/redfish/v1/...
```
Embedding `--noproxy *` inside a shell variable triggers glob expansion against cwd → curl tries to fetch local filenames through the proxy. See [pitfalls](./references/pitfalls.md).

Run flashing from a **third-party box** (not the host being flashed) — host SSH dies during chassis-off.

## Procedure

### 0. Pre-flight
```bash
BMC=10.123.233.190; HOST=10.123.233.191
BMC_USER=admin; BMC_PASS='zaq1@WSX'
export no_proxy='*' NO_PROXY='*'
CURL="curl -sk -u $BMC_USER:$BMC_PASS"
```
List current inventory + any in-progress task before doing anything:
```bash
$CURL https://$BMC/redfish/v1/UpdateService | python3 -m json.tool | grep -E 'FWUpdateInProgress|MultipartHttpPushUri'
$CURL https://$BMC/redfish/v1/TaskService/Tasks | python3 -m json.tool
$CURL https://$BMC/redfish/v1/UpdateService/FirmwareInventory | python3 -m json.tool
```

### 1. BIOS update (Redfish multipart push)
1. Power off chassis: `POST .../Systems/system/Actions/ComputerSystem.Reset` `{"ResetType":"ForceOff"}`, wait `PowerState=Off`, sleep 15s.
2. Push image. Use `Preserve=true` for BIOS only:
   ```bash
   $CURL -X POST https://$BMC/redfish/v1/UpdateService/upload \
     -F 'UpdateParameters={"Oem":{"QCT_IO":{"Preserve":true}}};type=application/json' \
     -F "UpdateFile=@/path/to/BIOS_3A16.tar.gz;type=application/octet-stream"
   ```
3. **Even on HTTP 400, the BMC may have started the update.** Always check `/UpdateService.Oem.QCT_IO.FWUpdateInProgress` and `/TaskService/Tasks` for a new Task ID. Don't retry blindly — you'll get 503.
4. Poll the Task until `TaskState=Completed`, `TaskStatus=OK`.
5. Power on: `{"ResetType":"On"}`. BIOS visible in `dmidecode -s bios-version` and `Systems/system.BiosVersion` only **~3 min after POST completes** (SMBIOS is cached).

### 2. BMC update
Push BMC tar.gz with `{}` UpdateParameters (no Preserve). BMC reboots automatically.
- New image lands in **previously-inactive bank**. `BMCPrimary` and `BMCSecondary` are dual A/B banks; live version = `/redfish/v1/Managers/bmc.FirmwareVersion`. Don't assume Primary is running.

### 3. CPLD (MPestiFPTarget / MPestiInitiator)
```bash
$CURL -X POST https://$BMC/redfish/v1/UpdateService/upload \
  -F 'UpdateParameters={};type=application/json' \
  -F "UpdateFile=@/path/to/CPLD_0103.tar.gz;type=application/octet-stream"
```
- **Do NOT send `Oem.QCT_IO.Preserve`** for CPLD → returns 400 `ActionParameterValueConflict`.
- **Do NOT send `@Redfish.OperationApplyTime`** anywhere → returns 400 `ActionParameterNotSupported`.
- Task completes "FirmwareUpdateCompleted to 0103" but inventory still shows `01.02`. **A full AC power cycle (cord pull, or BMC `Chassis.Reset PowerCycle`) is required** to activate front-panel CPLD. `ForceOff→On` is NOT enough.

### 4. CPU microcode (Ubuntu host, apt-managed)
```bash
sshpass -p r00tme ssh root@$HOST '
  apt-get update &&
  apt-get install -y intel-microcode &&
  dpkg-reconfigure intel-microcode &&
  update-initramfs -u &&
  reboot'
```
Verify after reboot:
```bash
grep -m1 microcode /proc/cpuinfo
dmesg | grep -i microcode | head
iucode_tool -tb -lS /lib/firmware/intel-ucode/$(printf '%02x-%02x-%02x' 6 173 1)
```
Notes:
- Package `intel-microcode 3.20260210.0ubuntu0.22.04.1` ships sig `0x000a06d1` pf_mask `0x95` rev `0x1000405`.
- "No upgrade available" simply means apt's revision == loaded revision.
- Confirm loader not disabled: kernel cmdline must NOT contain `dis_ucode_ldr`. Confirm package not held: `apt-mark showhold | grep -i microcode` should be empty.

### 5. Verify everything
Bundled one-shot check (preferred — no external repo needed):
```bash
.github/skills/gnr-sp-firmware-update/scripts/check_versions.sh \
  <BMC_IP> <HOST_IP> [BMC_USER] [BMC_PASS] [HOST_USER] [HOST_PASS]
# defaults: admin / zaq1@WSX / root / r00tme
# requires: curl, python3, ssh; auto-uses sshpass if installed
```
Reports: BMC live version, PowerState, FWUpdateInProgress, full FirmwareInventory (BIOS, BMCPrimary/Secondary, MPestiFPTarget, MPestiInitiator, FANCPLD, MBCPLD, SCMCPLD, PSU0), active Tasks, host BIOS (dmidecode), CPU sig + loaded microcode, `intel-microcode` package version, hold status, `dis_ucode_ldr` flag, recent microcode dmesg.

Alternative (upstream helper, interactive ssh prompt — send `r00tme`):
```bash
no_proxy='*' NO_PROXY='*' bash /root/awilczyn/Media-Transport-Library-Devtools/gnr_sp_platform/gnr_check_firmware.sh \
  $BMC $HOST $BMC_USER "$BMC_PASS" root
```
Expected good output (post-flash, post-AC-cycle):
```
=== Redfish (BMC ...) ===
  BIOS:           3A16.QCT001
  BMCPrimary:     3.08.00
  BMCSecondary:   3.20.00
  PowerState:     On
=== Host OS ===
  BIOS:           3A16.QCT001
  Microcode:      0x1000405
  dis_ucode_ldr:  NO
  ucode held:     NO
=== MPestiFPTarget ===
  Version= 01.03
```

## Key Gotchas (full list: [pitfalls](./references/pitfalls.md))
| Symptom | Cause | Fix |
|---|---|---|
| HTTP 400 `ActionParameterNotSupported` | Sent `@Redfish.OperationApplyTime` | Omit it; firmware applies on next reset/AC anyway |
| HTTP 400 `ActionParameterValueConflict` on CPLD | Sent `Oem.QCT_IO.Preserve` | Use `{}` for CPLD/BMC; only BIOS accepts `Preserve` |
| HTTP 503 `ServiceTemporarilyUnavailable` on retry | Previous 400 silently started a task | Check `Tasks` list before re-pushing |
| BMC `FirmwareVersion` doesn't change after upgrade | Image landed in opposite bank | Inspect both `BMCPrimary`/`BMCSecondary`; live = `Managers/bmc.FirmwareVersion` |
| MPestiFPTarget still old after reboot | FP CPLD needs AC power cycle | Cord pull or `Chassis.Reset PowerCycle` |
| `dmidecode` shows old BIOS post-flash | SMBIOS cached; BIOS visible ~3 min after POST | Wait, then re-check |
| afulnx_64 hangs at `4f - Error: Get block size error` | ROMID mismatch with new layout | Don't use afulnx on S7Q. Use Redfish. |
| curl fetches local filenames | `--noproxy *` glob-expanded inside `$CURL` | Use `no_proxy='*' NO_PROXY='*'` env vars |

## References
- [pitfalls.md](./references/pitfalls.md) — full lessons learned, error → cause → fix table
- [redfish-cheatsheet.md](./references/redfish-cheatsheet.md) — endpoints, payloads, polling snippets
- [scripts/check_versions.sh](./scripts/check_versions.sh) — one-shot BIOS/BMC/CPLD/microcode reporter
- Upstream firmware images & `gnr_check_firmware.sh`: <https://github.com/intel-sandbox/Media-Transport-Library-Devtools/tree/main/gnr_sp_platform>
