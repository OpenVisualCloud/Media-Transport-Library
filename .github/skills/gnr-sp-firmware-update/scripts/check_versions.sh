#!/usr/bin/env bash
# check_versions.sh — Report BIOS / BMC / CPLD / microcode versions on a GNR-SP S7Q host.
#
# Usage:
#   ./check_versions.sh <BMC_IP> <HOST_IP> [BMC_USER] [BMC_PASS] [HOST_USER] [HOST_PASS]
# Defaults: BMC_USER=admin BMC_PASS=zaq1@WSX HOST_USER=root HOST_PASS=r00tme
#
# Requires: curl, python3, ssh. Optional: sshpass (auto-uses if HOST_PASS set).
# Bypasses corp proxy via env vars (no glob-expansion bug).

set -u

BMC=${1:?BMC_IP required}
HOST=${2:?HOST_IP required}
BU=${3:-admin}
BP=${4:-zaq1@WSX}
HU=${5:-root}
HP=${6:-r00tme}

bold(){ printf '\033[1m%s\033[0m\n' "$*"; }
hr(){ printf -- '----------------------------------------\n'; }

: "${BMC_CURL_OPTS:=}"     # e.g. BMC_CURL_OPTS='--socks5-hostname proxy-mu.intel.com:1080'

# Bypass corp HTTP proxy ONLY when not routing via SOCKS — otherwise
# curl honors NO_PROXY '*' and refuses to use the SOCKS proxy too.
if [[ "$BMC_CURL_OPTS" != *socks* ]]; then
  export no_proxy='*' NO_PROXY='*'
else
  unset no_proxy NO_PROXY http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY
fi

rf() {
  # rf <path>  → curl Redfish, print JSON to stdout
  # shellcheck disable=SC2086
  curl -sk $BMC_CURL_OPTS --connect-timeout 8 -m 30 -u "$BU:$BP" "https://$BMC$1"
}

jget() {
  # jget <key-path>   reads JSON from stdin, prints d[key1][key2]...
  python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in sys.argv[1:]:
    if isinstance(d, list):
        try: d = d[int(k)]
        except: d = None; break
    elif isinstance(d, dict):
        d = d.get(k)
    else:
        d = None; break
print("" if d is None else d)
' "$@"
}

bold "=== BMC Redfish ($BMC) ==="
PING=$(rf /redfish/v1/ 2>&1)
if [[ -z "$PING" ]] || ! echo "$PING" | python3 -c 'import sys,json;json.load(sys.stdin)' >/dev/null 2>&1; then
  echo "  ERROR: cannot reach BMC Redfish at https://$BMC (check network/creds/proxy)"
else
  LIVE_BMC=$(rf /redfish/v1/Managers/bmc | jget FirmwareVersion)
  PSTATE=$(rf /redfish/v1/Systems/system | jget PowerState)
  FWIP=$(rf /redfish/v1/UpdateService | jget Oem QCT_IO FWUpdateInProgress)
  printf '  %-22s %s\n' 'BMC live (Managers/bmc):' "$LIVE_BMC"
  printf '  %-22s %s\n' 'PowerState:'              "$PSTATE"
  printf '  %-22s %s\n' 'FWUpdateInProgress:'      "${FWIP:-false}"
  echo
  bold "  FirmwareInventory:"
  for K in BIOS BMCPrimary BMCSecondary MPestiFPTarget MPestiInitiator FANCPLD MBCPLD SCMCPLD PSU0; do
    V=$(rf /redfish/v1/UpdateService/FirmwareInventory/$K 2>/dev/null | jget Version)
    [[ -n "$V" ]] && printf '    %-18s %s\n' "$K" "$V"
  done
  echo
  bold "  Active Tasks:"
  rf /redfish/v1/TaskService/Tasks | python3 -c '
import sys, json
d = json.load(sys.stdin)
ms = d.get("Members") or []
if not ms:
    print("    (none)"); sys.exit()
for m in ms:
    print("   ", m.get("@odata.id"))
'
fi

echo
hr
bold "=== Host OS ($HU@$HOST) ==="

if command -v sshpass >/dev/null 2>&1 && [[ -n "$HP" ]]; then
  SSH="sshpass -p $HP ssh"
else
  SSH="ssh"   # will prompt for password
fi

$SSH -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=no \
  "$HU@$HOST" 'bash -s' <<'REMOTE'
set -u
P(){ printf '  %-22s %s\n' "$1" "$2"; }
P 'Hostname:'        "$(hostname)"
P 'Kernel:'          "$(uname -r)"
P 'OS:'              "$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
P 'CPUs:'            "$(nproc)"
P 'Memory:'          "$(awk '/MemTotal/ {printf "%.0f GiB", $2/1024/1024}' /proc/meminfo)"
P 'Uptime:'          "$(uptime -p)"
echo
echo '  -- BIOS (dmidecode) --'
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo -n"; fi
P 'BIOS Version:'    "$($SUDO dmidecode -s bios-version 2>/dev/null)"
P 'BIOS Date:'       "$($SUDO dmidecode -s bios-release-date 2>/dev/null)"
P 'BIOS Vendor:'     "$($SUDO dmidecode -s bios-vendor 2>/dev/null)"
P 'Board:'           "$($SUDO dmidecode -s baseboard-product-name 2>/dev/null)"
P 'System:'          "$($SUDO dmidecode -s system-product-name 2>/dev/null)"
echo
echo '  -- CPU & Microcode --'
P 'CPU Model:'       "$(awk -F: '/model name/{print $2;exit}' /proc/cpuinfo | sed 's/^ *//')"
P 'CPU Sig:'         "$(python3 -c '
import re
d={}
for ln in open("/proc/cpuinfo"):
    m=re.match(r"(cpu family|model|stepping)\s*:\s*(\d+)", ln)
    if m: d[m.group(1)]=int(m.group(2))
    if len(d)==3: break
f,mo,s=d["cpu family"],d["model"],d["stepping"]
print("0x%08x  (family %d model %d stepping %d)"%(((f&0xff)<<8)|((mo&0xf0)<<12)|((mo&0x0f)<<4)|(s&0x0f),f,mo,s))')"
P 'Microcode loaded:' "$(awk -F: '/microcode/{print $2;exit}' /proc/cpuinfo | tr -d ' ')"
P 'Microcode (sysfs):' "$(cat /sys/devices/system/cpu/cpu0/microcode/version 2>/dev/null)"
P 'intel-microcode pkg:' "$(dpkg-query -W -f='${Version}' intel-microcode 2>/dev/null || echo 'not installed')"
P 'Pkg held:'        "$(apt-mark showhold 2>/dev/null | grep -q '^intel-microcode$' && echo YES || echo NO)"
P 'dis_ucode_ldr:'   "$(grep -qw dis_ucode_ldr /proc/cmdline && echo YES || echo NO)"
echo
echo '  -- Last microcode dmesg --'
dmesg 2>/dev/null | grep -i microcode | tail -3 | sed 's/^/    /'
REMOTE

echo
hr
bold "Done."
