#!/bin/bash
# sched_audit.sh — run bpftrace sched_switch audit on isolated cores
#                  then classify every observed task as:
#                    EXPECTED  — MTL/dpdk worker, idle
#                    ALLOWED   — kernel threads that may briefly appear
#                    INTRUDER  — should not be on isolated cores
#
# Usage:
#   sudo ./sched_audit.sh [options] [duration_sec] [cpu_min] [cpu_max]
#   sudo ./sched_audit.sh 5 2 3      # 5s sample on CPUs 2-3 (overrides auto-detect)
#   sudo ./sched_audit.sh 10          # 10s sample, auto-detect isolated range
#   sudo ./sched_audit.sh -h          # show help

set -euo pipefail

usage() {
    cat <<'HELPEOF'
Usage: sudo isolation_report.sh [options] [duration_sec] [cpu_min] [cpu_max]

Run a bpftrace sched_switch audit on isolated (or specified) CPU cores,
then classify every observed task as EXPECTED, ALLOWED, or INTRUDER.

POSITIONAL ARGUMENTS:
  duration_sec   Sampling duration in seconds            (default: 5)
  cpu_min        First CPU core to monitor               (default: auto-detect)
  cpu_max        Last CPU core to monitor                (default: auto-detect)

  If cpu_min and cpu_max are given they OVERRIDE the auto-detected
  isolcpus range.  If omitted, the script reads isolcpus from
  /sys/devices/system/cpu/isolated or /proc/cmdline.

OPTIONS:
  -h, --help     Show this help message and exit

EXAMPLES:
  sudo ./isolation_report.sh              # 5s, auto-detect isolated cores
  sudo ./isolation_report.sh 10           # 10s, auto-detect
  sudo ./isolation_report.sh 5 2 3        # 5s, force CPUs 2-3
  sudo ./isolation_report.sh 10 4 7       # 10s, force CPUs 4-7

PATTERN FILES (RC files):
  Every thread observed on the monitored cores is classified by matching
  its "comm" name (the 16-char Linux task name) against regex patterns
  loaded from two RC files next to this script:

    isolation_expected.rc   EXPECTED — threads that SHOULD be on these cores.
                            Your DPDK lcores, MTL sessions, your application
                            workers, idle threads, etc.  These are the
                            workload you intentionally pinned there.

    isolation_allowed.rc    ALLOWED — kernel housekeeping threads that MAY
                            briefly appear even on well-isolated cores
                            (migration, watchdog, ksoftirqd, kworker, ...).
                            Reported but not flagged as violations.
                            High counts here hint at incomplete isolation
                            (missing nohz_full=, rcu_nocbs=, etc.).

  Anything that matches neither file is classified as INTRUDER — a thread
  that should NOT be running on your isolated cores.

  Edit these files to add your own application threads or suppress known
  kernel threads that are acceptable in your environment.

  FORMAT:  One pattern per line.  Blank lines and lines starting with '#'
           are ignored.  Each pattern is a bash extended regex matched
           against the thread comm name with:  [[ "$comm" =~ $pattern ]]

  REGEX QUICK REFERENCE (bash =~ extended regex):
    ^           start of string           $           end of string
    .           any single character       \           escape next character
    *           0 or more of previous      +           1 or more of previous
    ?           0 or 1 of previous         {n,m}       n to m of previous
    [abc]       character class            [^abc]      negated class
    (a|b)       alternation                (...)       grouping
    \.  \-      literal dot / hyphen (escape special chars with backslash)

  PATTERN EXAMPLES:
    ^myapp$          exact match "myapp"
    ^myapp-worker    starts with "myapp-worker" (any suffix)
    ^(foo|bar)_      starts with "foo_" or "bar_"
    ^gst             any thread whose name begins with "gst"
    ^xcoder          match your transcoder threads
HELPEOF
    exit 0
}

# Handle help before anything else
case "${1:-}" in -h|--help) usage ;; esac

DURATION=${1:-5}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_LOG=$(mktemp /tmp/sched_audit_XXXXXX.log)
trap 'rm -f "$RAW_LOG"' EXIT

detect_isolated() {
    local iso
    iso=$(cat /sys/devices/system/cpu/isolated 2>/dev/null | tr -d '\n')
    [[ -z "$iso" ]] && iso=$(grep -oP 'isolcpus=\K\S+' /proc/cmdline 2>/dev/null)
    echo "$iso"
}

expand_cpulist() {
    python3 -c "
s='$1'; r=[]
for p in s.split(','):
    a,_,b=p.partition('-'); r.extend(range(int(a),int(b or a)+1))
print(min(r), max(r))
"
}

if [[ -n "${2:-}" && -n "${3:-}" ]]; then
    # Manual override — always wins
    CPU_MIN=$2
    CPU_MAX=$3
    echo "✓  Manual override — monitoring CPUs ${CPU_MIN}-${CPU_MAX}"
else
    ISO=$(detect_isolated)
    if [[ -z "$ISO" ]]; then
        CPU_MIN=${2:-2}
        CPU_MAX=${3:-3}
        echo "⚠  No isolcpus found — using CPU${CPU_MIN}-CPU${CPU_MAX}"
    else
        read -r CPU_MIN CPU_MAX <<< "$(expand_cpulist "$ISO")"
        echo "✓  Detected isolated cores: $ISO  (min=$CPU_MIN max=$CPU_MAX)"
    fi
fi

S="================================================================"
printf '\n%s\n  sched_switch audit  |  cores %d-%d  |  %ds sample\n%s\n\n' \
    "$S" "$CPU_MIN" "$CPU_MAX" "$DURATION" "$S"

# Load patterns from RC files (skip blank lines and comments)
load_patterns() {
    local file="$1"
    local -n arr=$2
    if [[ ! -f "$file" ]]; then
        echo "⚠  Pattern file not found: $file" >&2
        return
    fi
    while IFS= read -r line; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        arr+=("$line")
    done < "$file"
}

EXPECTED_PATTERNS=()
ALLOWED_PATTERNS=()
load_patterns "${SCRIPT_DIR}/isolation_expected.rc" EXPECTED_PATTERNS
load_patterns "${SCRIPT_DIR}/isolation_allowed.rc"  ALLOWED_PATTERNS

if (( ${#EXPECTED_PATTERNS[@]} == 0 )); then
    echo "⚠  No EXPECTED patterns loaded — all threads will be INTRUDER or ALLOWED" >&2
fi
if (( ${#ALLOWED_PATTERNS[@]} == 0 )); then
    echo "⚠  No ALLOWED patterns loaded" >&2
fi

classify() {
    local comm="$1"
    for pat in "${EXPECTED_PATTERNS[@]}"; do
        [[ "$comm" =~ $pat ]] && { echo "EXPECTED"; return; }
    done
    for pat in "${ALLOWED_PATTERNS[@]}"; do
        [[ "$comm" =~ $pat ]] && { echo "ALLOWED"; return; }
    done
    echo "INTRUDER"
}

BTRACE_PROG='
tracepoint:sched:sched_switch
/ cpu >= '"$CPU_MIN"' && cpu <= '"$CPU_MAX"' /
{
    @sw[cpu, args->next_comm, args->next_pid] = count();
}

interval:s:'"$DURATION"'
{
    print(@sw);
    clear(@sw);
    exit();
}
'

echo "Running bpftrace for ${DURATION}s..."
echo "(watching CPUs ${CPU_MIN}–${CPU_MAX} for sched_switch events)"
echo ""

sudo bpftrace -e "$BTRACE_PROG" 2>/dev/null | tee "$RAW_LOG"
printf '\n%s\n  CLASSIFICATION REPORT\n%s\n\n' "$S" "$S"
printf '  %-12s  %-4s  %-22s  %-8s  %s\n' \
    "VERDICT" "CPU" "COMM" "PID" "SWITCH_COUNT"
printf '  %s\n' "$(printf '%.0s-' {1..65})"
declare -A verdict_counts=([EXPECTED]=0 [ALLOWED]=0 [INTRUDER]=0)
intruder_list=()

while IFS= read -r line; do
    if [[ "$line" =~ @sw\[([0-9]+),\ ([^,]+),\ ([0-9]+)\]:\ ([0-9]+) ]]; then
        cpu="${BASH_REMATCH[1]}"
        comm="${BASH_REMATCH[2]}"
        pid="${BASH_REMATCH[3]}"
        cnt="${BASH_REMATCH[4]}"
        verdict=$(classify "$comm")
        verdict_counts[$verdict]=$(( ${verdict_counts[$verdict]} + cnt ))
        [[ "$verdict" == "INTRUDER" ]] && intruder_list+=("$comm (PID $pid, CPU $cpu, ${cnt}x)")
        printf '  %-12s  %-4s  %-22s  %-8s  %s\n' \
            "$verdict" "$cpu" "$comm" "$pid" "$cnt"
    fi
done < "$RAW_LOG"

printf '\n%s\n  SUMMARY\n%s\n' "$S" "$S"
total=$(( ${verdict_counts[EXPECTED]} + ${verdict_counts[ALLOWED]} + ${verdict_counts[INTRUDER]} ))
printf '\n  Total switches observed : %d\n' "$total"
printf '  EXPECTED switches    : %d\n' "${verdict_counts[EXPECTED]}"
printf '  ALLOWED  switches    : %d\n' "${verdict_counts[ALLOWED]}"
printf '  INTRUDER switches    : %d\n' "${verdict_counts[INTRUDER]}"

if (( ${#intruder_list[@]} > 0 )); then
    printf '\n  INTRUDERS:\n'
    for entry in "${intruder_list[@]}"; do
        printf '    X  %s\n' "$entry"
    done
    printf '\n Isolation violations detected!\n'
    printf '  Fix: check cpu affinity with taskset, cgroup cpuset,\n'
    printf '       or add nohz_full= to isolcpus kernel params.\n\n'
else
    printf '\n No intruders — isolated cores are clean.\n\n'
fi

printf '%s\n  Done\n%s\n\n' "$S" "$S"