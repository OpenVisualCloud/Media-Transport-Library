# OpenBMC Redfish Cheatsheet — QCT D55Q-2U S7Q

All examples assume:
```bash
BMC=10.123.233.190
U=admin; P='zaq1@WSX'
export no_proxy='*' NO_PROXY='*'
C="curl -sk -u $U:$P"
```

## Discovery
```bash
$C https://$BMC/redfish/v1/UpdateService | python3 -m json.tool
# Note: MultipartHttpPushUri = /redfish/v1/UpdateService/upload
# Note: Oem.QCT_IO.FWUpdateInProgress
```

## Power control
```bash
# Off
$C -X POST -H 'Content-Type: application/json' \
  -d '{"ResetType":"ForceOff"}' \
  https://$BMC/redfish/v1/Systems/system/Actions/ComputerSystem.Reset
# On
$C -X POST -H 'Content-Type: application/json' \
  -d '{"ResetType":"On"}' \
  https://$BMC/redfish/v1/Systems/system/Actions/ComputerSystem.Reset
# Full AC cycle (if supported)
$C -X POST -H 'Content-Type: application/json' \
  -d '{"ResetType":"PowerCycle"}' \
  https://$BMC/redfish/v1/Chassis/chassis/Actions/Chassis.Reset
# Read state
$C https://$BMC/redfish/v1/Systems/system | python3 -c 'import sys,json;print(json.load(sys.stdin)["PowerState"])'
```

## Firmware inventory
```bash
$C https://$BMC/redfish/v1/UpdateService/FirmwareInventory | python3 -m json.tool
# Members typically include:
#   BIOS, BMCPrimary, BMCSecondary,
#   MPestiFPTarget, MPestiInitiator,
#   FANCPLD, MBCPLD, SCMCPLD, PSU0
for k in BIOS BMCPrimary BMCSecondary MPestiFPTarget MPestiInitiator; do
  v=$($C https://$BMC/redfish/v1/UpdateService/FirmwareInventory/$k | python3 -c 'import sys,json;print(json.load(sys.stdin).get("Version"))')
  printf '%-18s %s\n' "$k" "$v"
done
# Live BMC version (the only trustworthy one):
$C https://$BMC/redfish/v1/Managers/bmc | python3 -c 'import sys,json;print(json.load(sys.stdin)["FirmwareVersion"])'
```

## Multipart push
```bash
# BIOS
$C -X POST https://$BMC/redfish/v1/UpdateService/upload \
  -F 'UpdateParameters={"Oem":{"QCT_IO":{"Preserve":true}}};type=application/json' \
  -F "UpdateFile=@$PWD/BIOS_3A16.tar.gz;type=application/octet-stream"
# BMC / CPLD (no Preserve, no ApplyTime)
$C -X POST https://$BMC/redfish/v1/UpdateService/upload \
  -F 'UpdateParameters={};type=application/json' \
  -F "UpdateFile=@$PWD/CPLD_0103.tar.gz;type=application/octet-stream"
```

## Task polling
```bash
$C https://$BMC/redfish/v1/TaskService/Tasks | python3 -m json.tool
TID=123
while :; do
  read -r ST SS PCT < <($C https://$BMC/redfish/v1/TaskService/Tasks/$TID | \
    python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["TaskState"],d.get("TaskStatus"),d.get("PercentComplete",""))')
  printf '%s  %-12s %-4s %s%%\n' "$(date +%T)" "$ST" "$SS" "$PCT"
  case "$ST" in Completed|Exception|Killed|Cancelled) break;; esac
  sleep 5
done
```

## Pre-flight safety check (before any retry)
```bash
$C https://$BMC/redfish/v1/UpdateService | python3 -c '
import sys,json;d=json.load(sys.stdin)
print("FWUpdateInProgress:", d.get("Oem",{}).get("QCT_IO",{}).get("FWUpdateInProgress"))'
$C https://$BMC/redfish/v1/TaskService/Tasks | python3 -m json.tool
```

## Host-side verification (Ubuntu)
```bash
dmidecode -s bios-version
dmidecode -s bios-release-date
grep -m1 microcode /proc/cpuinfo
dmesg | grep -i microcode
journalctl -k | grep -i 'microcode\|bios' | tail
cat /sys/devices/system/cpu/cpu0/microcode/version
```
