#!/bin/bash
# sched_audit.sh — run bpftrace sched_switch audit on isolated cores
#                  then classify every observed task as:
#                    EXPECTED  — MTL/dpdk worker, idle
#                    ALLOWED   — kernel threads that may briefly appear
#                    INTRUDER  — should not be on isolated cores
#
# Usage:
#   sudo ./sched_audit.sh [options] [duration_sec] [cpu_spec...]
#   sudo ./sched_audit.sh 5 2 3      # 5s sample on CPUs 2-3 (range)
#   sudo ./sched_audit.sh 5 2-7      # 5s sample on CPUs 2-7 (range notation)
#   sudo ./sched_audit.sh 5 2 5 7    # 5s sample on CPUs 2, 5, 7 (discrete)
#   sudo ./sched_audit.sh 5 2-4,7    # 5s sample on CPUs 2,3,4,7 (mixed)
#   sudo ./sched_audit.sh 10         # 10s sample, auto-detect isolated range
#   sudo ./sched_audit.sh -h         # show help

set -euo pipefail

usage() {
	cat <<'HELPEOF'
Usage: sudo isolation_report.sh [options] [duration_sec] [cpu_spec...]

Run a bpftrace sched_switch audit on isolated (or specified) CPU cores,
then classify every observed task as EXPECTED, ALLOWED, or INTRUDER.

POSITIONAL ARGUMENTS:
  duration_sec   Sampling duration in seconds            (default: 5)
  cpu_spec       CPU cores to monitor. Supports:
                   2 7         range min-max (legacy)
                   2-7         range notation
                   2,5,7       comma-separated
                   2-4,7,9-11  mixed ranges and singles
                   2 5 7       space-separated discrete list
                 (default: auto-detect from isolcpus)

OPTIONS:
  -h, --help     Show this help message and exit

EXAMPLES:
  sudo ./isolation_report.sh              # 5s, auto-detect isolated cores
  sudo ./isolation_report.sh 10           # 10s, auto-detect
  sudo ./isolation_report.sh 5 2 3        # 5s, force CPUs 2-3 (range)
  sudo ./isolation_report.sh 5 2-7        # 5s, force CPUs 2-7
  sudo ./isolation_report.sh 5 2 5 7      # 5s, force CPUs 2,5,7 (discrete)
  sudo ./isolation_report.sh 5 2-4,7      # 5s, CPUs 2,3,4,7

PATTERN FILES (RC files):
  Every thread observed on the monitored cores is classified by matching
  its "comm" name (the 16-char Linux task name) against regex patterns
  loaded from two RC files next to this script:

    isolation_expected.env  EXPECTED — threads that SHOULD be on these cores.
                            Your DPDK lcores, MTL sessions, your application
                            workers, idle threads, etc.  These are the
                            workload you intentionally pinned there.

    isolation_allowed.env   ALLOWED — kernel housekeeping threads that MAY
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
case "${1:-}" in -h | --help) usage ;; esac

DURATION=${1:-5}
if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || ((DURATION < 1)); then
	echo "Error: duration must be a positive integer (got: '$DURATION')" >&2
	exit 1
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_LOG=$(mktemp /tmp/sched_audit_XXXXXX.log)
trap 'rm -f "$RAW_LOG"' EXIT

detect_isolated() {
	local iso
	iso=$(tr -d '\n' </sys/devices/system/cpu/isolated 2>/dev/null)
	[[ -z "$iso" ]] && iso=$(grep -oP 'isolcpus=\K\S+' /proc/cmdline 2>/dev/null)
	echo "$iso"
}

# Expand a cpulist spec (e.g. "2-4,7,9-11") into a sorted, deduplicated
# space-separated list of individual CPU numbers.
expand_cpulist() {
	python3 -c "
import sys
s='$1'; r=set()
for p in s.split(','):
    p=p.strip()
    if not p: continue
    a,_,b=p.partition('-')
    r.update(range(int(a),int(b or a)+1))
print(' '.join(str(x) for x in sorted(r)))
"
}

# Build CPU_LIST (space-separated individual CPUs) from arguments
if [[ -n "${2:-}" ]]; then
	# Join all args after duration with commas, then expand
	raw="${*:2}"
	# Replace spaces with commas so "2 5 7" becomes "2,5,7"
	raw="${raw// /,}"
	read -ra CPU_LIST <<<"$(expand_cpulist "$raw")"
	echo "✓  Manual override — monitoring CPUs: ${CPU_LIST[*]}"
else
	ISO=$(detect_isolated)
	if [[ -z "$ISO" ]]; then
		CPU_LIST=(2 3)
		echo "⚠  No isolcpus found — using CPUs: ${CPU_LIST[*]}"
	else
		read -ra CPU_LIST <<<"$(expand_cpulist "$ISO")"
		echo "✓  Detected isolated cores: $ISO  → CPUs: ${CPU_LIST[*]}"
	fi
fi

# Derive min/max for display
CPU_MIN=${CPU_LIST[0]}
CPU_MAX=${CPU_LIST[-1]}

# Build bpftrace CPU filter expression
if ((CPU_MAX - CPU_MIN + 1 == ${#CPU_LIST[@]})); then
	# Contiguous range — use efficient >= && <=
	BPF_CPU_FILTER="cpu >= $CPU_MIN && cpu <= $CPU_MAX"
else
	# Non-contiguous — use explicit || checks
	filter_parts=()
	for c in "${CPU_LIST[@]}"; do
		filter_parts+=("cpu == $c")
	done
	BPF_CPU_FILTER=$(IFS='|'; echo "${filter_parts[*]}" | sed 's/|/ || /g')
fi

S="================================================================"
printf '\n%s\n  sched_switch audit  |  cores [%s]  |  %ds sample\n%s\n\n' \
	"$S" "${CPU_LIST[*]}" "$DURATION" "$S"

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
	done <"$file"
}

EXPECTED_PATTERNS=()
ALLOWED_PATTERNS=()
load_patterns "${SCRIPT_DIR}/isolation_expected.env" EXPECTED_PATTERNS
load_patterns "${SCRIPT_DIR}/isolation_allowed.env" ALLOWED_PATTERNS

if ((${#EXPECTED_PATTERNS[@]} == 0)); then
	echo "⚠  No EXPECTED patterns loaded — all threads will be INTRUDER or ALLOWED" >&2
fi
if ((${#ALLOWED_PATTERNS[@]} == 0)); then
	echo "⚠  No ALLOWED patterns loaded" >&2
fi

classify() {
	local comm="$1"
	for pat in "${EXPECTED_PATTERNS[@]}"; do
		[[ "$comm" =~ $pat ]] && {
			echo "EXPECTED"
			return
		}
	done
	for pat in "${ALLOWED_PATTERNS[@]}"; do
		[[ "$comm" =~ $pat ]] && {
			echo "ALLOWED"
			return
		}
	done
	echo "INTRUDER"
}

BTRACE_PROG='
tracepoint:sched:sched_switch
/ '"$BPF_CPU_FILTER"' /
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
echo "(watching CPUs [${CPU_LIST[*]}] for sched_switch events)"
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
		verdict_counts[$verdict]=$((${verdict_counts[$verdict]} + cnt))
		[[ "$verdict" == "INTRUDER" ]] && intruder_list+=("$comm (PID $pid, CPU $cpu, ${cnt}x)")
		printf '  %-12s  %-4s  %-22s  %-8s  %s\n' \
			"$verdict" "$cpu" "$comm" "$pid" "$cnt"
	fi
done <"$RAW_LOG"

printf '\n%s\n  SUMMARY\n%s\n' "$S" "$S"
total=$((verdict_counts[EXPECTED] + verdict_counts[ALLOWED] + verdict_counts[INTRUDER]))
printf '\n  Total switches observed : %d\n' "$total"
printf '  EXPECTED switches    : %d\n' "${verdict_counts[EXPECTED]}"
printf '  ALLOWED  switches    : %d\n' "${verdict_counts[ALLOWED]}"
printf '  INTRUDER switches    : %d\n' "${verdict_counts[INTRUDER]}"

if ((${#intruder_list[@]} > 0)); then
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
