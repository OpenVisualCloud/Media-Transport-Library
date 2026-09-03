#!/usr/bin/env bash
# Verify that a worker's BIOS is set up the way this lab expects.
#
# BIOS cannot be read directly from Linux, but every setting that matters for
# this workload has an observable consequence in the running kernel. This script
# checks those consequences on the worker over SSH and prints the BIOS menu item
# to change for each mismatch. Those menu paths use one vendor's wording as an
# example - other vendors name and nest the same settings differently, so match
# on the effect. Read-only; changes nothing.
#
# Usage:
#   scripts/check-bios.sh [NODE]     from the controller (default: LAB_DEFAULT_NODE)
#   scripts/check-bios.sh --local    on the worker itself; no SSH, no config needed
# Full setting list and rationale: docs/01-bios-bkc.md
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LOCAL=0
[[ "${1:-}" == "--local" ]] && { LOCAL=1; shift; }

# lab.env is committed, so it is readable in both modes and tells the SMT check
# what this lab was configured for. Only the inventory is mode-specific.
# shellcheck source=/dev/null
[[ -r "$ROOT/config/lab.env" ]] && source "$ROOT/config/lab.env"
# shellcheck source=lib/perfspect.sh
source "$ROOT/scripts/lib/perfspect.sh"

if (( LOCAL )); then
  # Checking the machine you are sitting on: nothing here needs the inventory,
  # so this works on a worker before any of the lab is configured.
  echo "== BIOS BKC check for $(hostname) (this host) =="
else
  # shellcheck source=/dev/null
  source "$ROOT/config/nodes.env"
  : "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env (or run it on the worker with --local)}"

  NODE="${1:-$LAB_DEFAULT_NODE}"
  KEY="${NODE^^}_HOST"; KEY="${KEY//-/_}"
  HOST="${!KEY:-}"
  [[ -n "$HOST" ]] || { echo "FATAL: no address for $NODE ($KEY) in config/nodes.env" >&2; exit 2; }
  echo "== BIOS BKC check for $NODE ($HOST) =="
fi

CHECKS="$(cat <<'REMOTE'
set -u
fail=0
ok()   { printf '  OK    %-34s %s\n' "$1" "$2"; }
bad()  { printf '  WRONG %-34s %s\n' "$1" "$2"; printf '        BIOS: %s\n' "$3"; fail=1; }
warn() { printf '  WARN  %-34s %s\n' "$1" "$2"; }
info() { printf '  info  %-34s %s\n' "$1" "$2"; }

printf '  %-40s %s\n' 'System:' "$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null) $(cat /sys/class/dmi/id/product_name 2>/dev/null)"
printf '  %-40s %s\n' 'BIOS:' "$(cat /sys/class/dmi/id/bios_version 2>/dev/null) ($(cat /sys/class/dmi/id/bios_date 2>/dev/null))"
printf '  %-40s %s\n' 'CPU:' "$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)"
echo

# 1. Hyper-Threading may be on or off, but config/lab.env has to agree with the
#    hardware: DEC_CORES and ENC_CORES are physical cores, and the CPU request
#    the kubelet admits under full-pcpus-only is cores x LAB_THREADS_PER_CORE.
#    A disagreement either fails admission with SMTAlignmentError or hands a
#    container half the cores the scenario claims, so the density is not
#    comparable. HT off remains the simpler configuration to reason about.
threads=$(lscpu | awk -F: '/^Thread\(s\) per core/{gsub(/ /,"",$2);print $2}')
if [[ -z "$LAB_EXPECTED_THREADS" ]]; then
  warn "Threads per core" "$threads observed; set LAB_THREADS_PER_CORE=$threads in config/lab.env"
elif [[ "$threads" == "$LAB_EXPECTED_THREADS" ]]; then
  if [[ "$threads" == "1" ]]; then ok "Hyper-Threading disabled" "1 thread per core, as configured"
  else ok "Hyper-Threading enabled" "$threads threads per core, as configured (LAB_THREADS_PER_CORE=$threads)"; fi
else
  bad "Threads per core" "$threads observed, LAB_THREADS_PER_CORE=$LAB_EXPECTED_THREADS" \
      "Processor Settings -> Logical Processor (or set LAB_THREADS_PER_CORE=$threads in config/lab.env)"
fi

# 2. Sub-NUMA Clustering must be OFF: the CPU-pool planner and the
#    single-numa-node Topology Manager assume exactly one NUMA node per socket.
sockets=$(lscpu | awk -F: '/^Socket\(s\)/{gsub(/ /,"",$2);print $2}')
nodes=$(lscpu | awk -F: '/^NUMA node\(s\)/{gsub(/ /,"",$2);print $2}')
if [[ "$nodes" == "$sockets" ]]; then ok "Sub-NUMA Clustering disabled" "$nodes NUMA nodes for $sockets sockets"
else bad "NUMA node count" "$nodes nodes for $sockets sockets" "Memory Settings -> Sub-NUMA Cluster = Disabled"; fi

# 3. NUMA must be enabled at all (interleaved memory would hide all locality).
if [[ "$nodes" -ge 2 ]]; then ok "NUMA enabled" "$nodes nodes"
else bad "NUMA" "only $nodes node" "Memory Settings -> Node Interleaving = Disabled"; fi

# 4. Turbo must be ON: the encoder is latency-sensitive and relies on turbo
#    frequency to hold 60 FPS at the medium preset.
if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
  if [[ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)" == "0" ]]; then ok "Turbo Boost enabled" "intel_pstate no_turbo=0"
  else bad "Turbo Boost" "intel_pstate no_turbo=1" "Processor Settings -> Turbo Boost = Enabled"; fi
else warn "Turbo Boost" "intel_pstate not present, cannot verify"; fi

# 5. Deep C-states must be OFF: C6 exit latency shows up directly as dropped
#    frames when a pinned encoder core idles between frames.
states=$(ls -d /sys/devices/system/cpu/cpu0/cpuidle/state* 2>/dev/null | wc -l)
if [[ "$states" -le 2 ]]; then ok "Deep C-states disabled" "$states idle states exposed"
else warn "C-states" "$states idle states exposed; C6+ may add wake-up latency" ; fi

if [[ "$(id -u)" -eq 0 ]]; then admin=(); else admin=(sudo -n); fi

# 6. Performance power profile: the intel_pstate driver mode, the OS governor and
#    the two energy-performance hints. None of these is set in BIOS alone -
#    scripts/configure-power.sh applies them from config/lab.env and keeps them
#    across a reboot, and its --verify enforces them. This only reports; PerfSpect
#    is the authority for EPB source selection/reporting.
want_pstate="${LAB_EXPECTED_PSTATE:-active}"
driver=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo unknown)
pstate=$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null || echo absent)
if [[ "$want_pstate" == "skip" ]]; then warn "P-state driver" "$driver, status=$pstate (not managed by this lab)"
elif [[ "$pstate" == "$want_pstate" ]]; then ok "P-state driver" "$driver, status=$pstate"
elif [[ "$pstate" == "absent" ]]; then warn "P-state driver" "$driver (no intel_pstate on this kernel)"
else bad "P-state driver" "status=$pstate, expected $want_pstate" \
    "not BIOS, a kernel setting: scripts/configure-power.sh <node>"; fi

cfg_value() {
  local key="$1" line value
  line=$(printf '%s\n' "$perfspect_out" | grep -F -m1 "$key:" || true)
  [[ -n "$line" ]] || return 1
  value="${line#*:}"
  printf '%s\n' "$value" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+--.*$//; s/[[:space:]]+$//'
}
cfg_num() {
  printf '%s\n' "$1" | sed -nE 's/.*\(([0-9]+)\)$/\1/p'
}

perfspect_bin="$HOME/${LAB_PERFSPECT_DIR_REMOTE:-perfspect}/perfspect"
want_gov="${LAB_EXPECTED_GOV:-performance}"
want_epb="${LAB_EXPECTED_EPB:-0}"
want_epp="${LAB_EXPECTED_EPP:-0}"
if [[ ! -x "$perfspect_bin" ]]; then
  warn "PerfSpect power checks" "PerfSpect not installed ($perfspect_bin). Run scripts/configure-power.sh <node> from the controller to install it and report EPB/EPP/ELC."
elif ! perfspect_out=$("${admin[@]}" "$perfspect_bin" config --noupdate 2>/dev/null); then
  warn "PerfSpect power checks" "cannot run PerfSpect config non-interactively as root. Run scripts/configure-power.sh <node> from the controller (installs/uses PerfSpect), or run this check with sudo."
else
  gov=$(cfg_value "Scaling Governor" || true)
  if [[ -z "$gov" ]]; then warn "CPU governor" "PerfSpect did not report it"
  elif [[ "$gov" == "$want_gov" ]]; then ok "CPU governor" "$gov (PerfSpect)"
  else bad "CPU governor" "$gov - expected $want_gov" \
      "System Profile = Performance (then: scripts/configure-power.sh <node>)"; fi

  epb=$(cfg_value "Energy Performance Bias" || true)
  epb_num=$(cfg_num "$epb")
  if [[ -z "$epb" ]]; then warn "Energy-performance bias" "PerfSpect did not report it"
  elif [[ -z "$epb_num" ]]; then warn "Energy-performance bias" "unreadable PerfSpect value: $epb"
  elif [[ "$epb_num" == "$want_epb" ]]; then ok "Energy-performance bias" "$epb (PerfSpect-selected source)"
  else warn "Energy-performance bias" "$epb - expected $want_epb; scripts/configure-power.sh sets it"; fi

  # Reported, never failed on.
  epp=$(cfg_value "Energy Performance Preference" || true)
  if [[ -z "$epp" ]]; then warn "Energy-perf preference" "PerfSpect did not report it (unsupported/unreadable on this platform)"
  else info "Energy-perf preference" "$epp (requested $want_epp)"; fi

  elc=$(cfg_value "Efficiency Latency Control" || true)
  if [[ -z "$elc" ]]; then warn "Efficiency Latency Control" "PerfSpect did not report it (expected on pre-Sierra Forest CPUs)"
  else info "Efficiency Latency Control" "$elc (requested ${LAB_EXPECTED_ELC:-latency})"; fi
fi

# 7. Intel RDT must be enabled in BIOS or resctrl exposes nothing to allocate.
flags=$(sed -n 's/^flags[[:space:]]*: //p' /proc/cpuinfo | head -1)
for f in rdt_a cat_l3 mba cqm_occup_llc cqm_mbm_total cqm_mbm_local; do
  case " $flags " in
    *" $f "*) ok "CPU feature $f" "present" ;;
    *)        bad "CPU feature $f" "absent" "Processor Settings -> Intel(R) RDT / Cache Allocation / Memory Bandwidth Allocation = Enabled" ;;
  esac
done

# 8. resctrl details the RDT profiles depend on.
if [[ -d /sys/fs/resctrl/info/L3 ]]; then
  ways=$(cat /sys/fs/resctrl/info/L3/cbm_mask)
  ok "L3 CAT ways (cbm_mask)" "$ways, min_cbm_bits=$(cat /sys/fs/resctrl/info/L3/min_cbm_bits), CLOS=$(cat /sys/fs/resctrl/info/L3/num_closids)"
  [[ "$(cat /sys/fs/resctrl/info/L3/min_cbm_bits)" == "1" ]] || warn "cat-16-1 profile" "needs min_cbm_bits=1"
else
  warn "resctrl" "not mounted yet - chapter 6 does it (scripts/bootstrap-worker.sh)"
fi
if [[ -d /sys/fs/resctrl/info/MB ]]; then
  ok "MBA granularity" "min=$(cat /sys/fs/resctrl/info/MB/min_bandwidth) gran=$(cat /sys/fs/resctrl/info/MB/bandwidth_gran)"
fi

# 9. All memory channels populated. Half-populated DIMMs halve peak bandwidth
#    and silently change every bandwidth number in the report.
if "${admin[@]}" /usr/sbin/dmidecode -t memory >/dev/null 2>&1; then
  populated=$("${admin[@]}" /usr/sbin/dmidecode -t memory | grep -c '^	Size: [0-9]')
  speed=$("${admin[@]}" /usr/sbin/dmidecode -t memory | sed -n 's/^\tConfigured Memory Speed: \([0-9]*\).*/\1/p' | head -1)
  ok "Populated DIMMs" "$populated at ${speed:-unknown} MT/s"
else
  warn "DIMM population" "DMI needs a sudo rule - chapter 6 adds it (scripts/bootstrap-worker.sh)"
fi

# 10. /dev/shm must be large enough for the MXL shared-memory flows.
shm=$(df -BG --output=size /dev/shm 2>/dev/null | tail -1 | tr -dc 0-9)
if [[ "${shm:-0}" -ge 32 ]]; then ok "/dev/shm size" "${shm} GiB (MXL flows live here)"
else bad "/dev/shm size" "${shm:-0} GiB" "not BIOS: mount -o remount,size=50%% /dev/shm"; fi

echo
if [[ "$fail" -eq 0 ]]; then echo "BIOS BKC check passed."; else echo "BIOS BKC check found mismatches (WRONG lines above)." >&2; exit 1; fi
REMOTE
)"

# The check script is a quoted heredoc, so nothing from here leaks into it by
# accident; what it may know is passed in deliberately, in front of it. Each of
# these is empty when lab.env is unreadable (--local on an unconfigured worker),
# and the checks then fall back to the reference BKC value.
PRELUDE="$(cat <<PRE
LAB_EXPECTED_THREADS='${LAB_THREADS_PER_CORE:-}'
LAB_EXPECTED_PSTATE='${LAB_POWER_PSTATE_DRIVER:-}'
LAB_EXPECTED_GOV='${LAB_POWER_GOVERNOR:-}'
LAB_EXPECTED_EPB='${LAB_POWER_EPB:-}'
LAB_EXPECTED_EPP='${LAB_POWER_EPP:-}'
LAB_EXPECTED_ELC='${LAB_POWER_ELC:-}'
LAB_PERFSPECT_DIR_REMOTE='${LAB_PERFSPECT_DIR:-perfspect}'
PRE
)"

if (( LOCAL )); then
  printf '%s\n%s\n' "$PRELUDE" "$CHECKS" | bash -s
else
  printf '%s\n%s\n' "$PRELUDE" "$CHECKS" | ssh -o BatchMode=yes -o ConnectTimeout=10 "${LAB_SSH_USER}@$HOST" 'bash -s'
fi
