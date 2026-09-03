# GNR-SP S7Q Firmware Update — Pitfalls & Lessons Learned

Verified on QuantaGrid D55Q-2U S7Q, OpenBMC 3.08→3.20, BIOS 3A09→3A16, MPestiFPTarget 01.02→01.03, microcode 0x1000405 (sig 0x000a06d1).

## Network / Proxy

### Glob-expansion bug with `--noproxy *`
**Bad:**
```bash
CURL="curl -sk --noproxy * -u admin:pw"
$CURL https://bmc/...   # * expands against cwd → curl fetches local filenames through proxy
```
**Good:**
```bash
export no_proxy='*' NO_PROXY='*'
curl -sk -u admin:pw https://bmc/...
# OR literal-on-command-line:
curl -sk --noproxy '*' -u admin:pw https://bmc/...
```

### Run from a third-party box
Flashing from the host being flashed kills your SSH the moment chassis goes Off. Always drive Redfish from a separate machine that has L3 reachability to the BMC IP.

### BMC subnet only reachable via SOCKS5 (`134.191.216.x`)
Some lab BMC subnets are not directly routable; access is via Intel SOCKS proxy.
- Check `~/.ssh/config` for an entry like `ProxyCommand /usr/bin/nc -X 5 -x proxy-mu.intel.com:1080 %h %p` — if present, curl needs `--socks5-hostname proxy-mu.intel.com:1080`.
- **Trap:** `NO_PROXY='*'` makes curl bypass the SOCKS proxy too. Either unset `*_proxy`/`NO_PROXY` env vars or omit the no_proxy export when going via SOCKS.
- `check_versions.sh` honors `BMC_CURL_OPTS` env var:
  ```bash
  BMC_CURL_OPTS='--socks5-hostname proxy-mu.intel.com:1080' \
    ./check_versions.sh 134.191.216.119 134.191.216.126 awilczyn 'PW' root r00tme
  ```
- ipmitool over LAN won't work via SOCKS (UDP). Fall back to SSH into the BMC and run tools locally (OpenBMC: `ssh awilczyn@<bmc>`; user must be in `priv-admin`).


## Redfish UpdateService payload constraints

Multipart push URI: `POST /redfish/v1/UpdateService/upload` (the value of `MultipartHttpPushUri`).

| Field | BIOS | BMC | CPLD (MPesti*) |
|---|---|---|---|
| `Oem.QCT_IO.Preserve: true` | ✅ supported | ❌ 400 | ❌ 400 ActionParameterValueConflict |
| `@Redfish.OperationApplyTime` | ❌ 400 ActionParameterNotSupported | ❌ 400 | ❌ 400 |
| Empty `{}` | ✅ | ✅ | ✅ |

→ Use minimum viable JSON: `{"Oem":{"QCT_IO":{"Preserve":true}}}` only for BIOS, `{}` for everything else.

### Silent updates on HTTP 400
The BMC parses the multipart body, validates each parameter independently, returns 400 for the offending param, **and may still kick off the firmware task using the file you uploaded**. If you immediately retry, you get HTTP 503 `ServiceTemporarilyUnavailable` because the silent task is hogging the update path.

**Always before retrying any failed POST:**
```bash
curl -sk -u "$U:$P" "https://$BMC/redfish/v1/UpdateService" | grep FWUpdateInProgress
curl -sk -u "$U:$P" "https://$BMC/redfish/v1/TaskService/Tasks"
```
If a new Task ID exists, poll it instead of retrying.

### Task polling skeleton
```bash
TASK=/redfish/v1/TaskService/Tasks/123
while :; do
  S=$(curl -sk -u "$U:$P" "https://$BMC$TASK" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["TaskState"],d.get("TaskStatus"),d.get("PercentComplete"))')
  echo "$(date +%T) $S"
  case "$S" in Completed*|Exception*|Killed*|Cancelled*) break;; esac
  sleep 5
done
```

## Inventory peculiarities

- **Dual-bank BMC**: `FirmwareInventory/BMCPrimary` and `BMCSecondary` are A/B. Upgrade lands in the inactive bank → `Primary` field name is misleading. The running version is **only** trustworthy from `/redfish/v1/Managers/bmc.FirmwareVersion`.
- **BIOS visibility lag**: After a successful BIOS task + Power On, `dmidecode -s bios-version`, `Systems/system.BiosVersion`, and `FirmwareInventory/BIOS.Version` keep showing the old version until POST finishes (~3 min). Don't panic, don't reflash.
- **CPLD activation**: `MPestiFPTarget` and `MPestiInitiator` need a **full AC power cycle** (cord pull or `Chassis.Reset PowerCycle` if BMC supports it). `ComputerSystem.Reset ForceOff→On` keeps standby power → CPLD doesn't reload → inventory still old. Confirm uptime is short (`uptime` reports minutes) after the AC cycle.

## Avoid: in-OS BIOS flashing (afulnx_64)

afulnx_64 5.16.04.0135 on S7Q with BIOS 3A09→3A16 fails:
```
Reading flash ............... done
ROM File ROMID is not compatible with existing BIOS ROMID.
SMI driver hang at "4f - Error: Get block size error"
```
Root cause: ROM layout changed between revisions; afulnx ROMID check is stricter than the in-band SMI handler can tolerate. **Use Redfish multipart push instead** — it is the supported path on this BMC.

## Microcode (Ubuntu)

- `intel-microcode` from `jammy-updates`/`jammy-security` is the canonical source.
- "Already up to date" / no apt upgrade ≠ stale microcode. The package may already ship the same revision the kernel loaded at boot. Compare:
  ```bash
  grep -m1 microcode /proc/cpuinfo                                    # currently loaded
  iucode_tool -tb -lS /lib/firmware/intel-ucode/06-ad-01 | tail -5    # what apt provides
  ```
- After install: `dpkg-reconfigure intel-microcode && update-initramfs -u && reboot`.
- Verify loader is enabled: kernel cmdline must NOT contain `dis_ucode_ldr`.
- Verify package not pinned: `apt-mark showhold | grep -i microcode` should print nothing.
- If lab image had `dis_ucode_ldr` baked into `/etc/default/grub`:
  ```bash
  cp /etc/default/grub /etc/default/grub.bak.$(date +%s)
  sed -i 's/ *dis_ucode_ldr//g' /etc/default/grub
  update-grub
  apt-mark unhold intel-microcode
  dpkg-reconfigure intel-microcode
  update-initramfs -u
  reboot
  ```
  Recovery grub entry may still keep `dis_ucode_ldr` — harmless.

## Boot order — fix from Ubuntu, not BMC

QCT factory `BootOrder` buries the disk (e.g. `Boot0002` ubuntu) **last**, behind every NIC's HTTP-IPv4/PXE-IPv4/HTTP-IPv6/PXE-IPv6 entry. After every cold reboot the host spends 20–40 min cycling failed PXE attempts before reaching disk.

- **Redfish PATCH does NOT work**: `/Systems/system.Boot.BootOrder` returns `PropertyUnknown`; `/Systems/system/SD` (Settings) same; `BootSourceOverrideEnabled=Continuous` returns HTTP 403 `InsufficientPrivilege` even with `RoleId=Administrator` on this bmcweb build.
- **Fix from inside Ubuntu** (writes EFI variables directly, no BMC required):
  ```bash
  efibootmgr   # find disk entry (e.g. Boot0002 ubuntu)
  efibootmgr -o 0002,0001,0003,0004,0005,0006,0007,0008,0009,000A,000B,000C,000D,000E,000F,0010,0011,0012
  ```
- After this, next reboot goes straight to GRUB in seconds.


## Verified end-to-end flow (BIOS + CPLD)

1. `ForceOff` → wait `PowerState=Off` → sleep 15s
2. POST `/UpdateService/upload` BIOS tar.gz with `{"Oem":{"QCT_IO":{"Preserve":true}}}`
3. Check Tasks list (silent-update guard) → poll new task → wait `Completed/OK`
4. POST `/UpdateService/upload` CPLD tar.gz with `{}`
5. Same: check Tasks → poll → `Completed/OK`
6. `ResetType=On` → wait `PowerState=On`
7. Sleep ~3 min → verify BIOS via `dmidecode` and `Systems/system.BiosVersion`
8. **Schedule full AC power cycle** to activate CPLD; re-check `MPestiFPTarget.Version` after.
