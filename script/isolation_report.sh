#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Run a bpftrace sched_switch audit on isolated cores, then classify every
# observed task as:
#   EXPECTED  - MTL/DPDK worker, idle
#   ALLOWED   - kernel threads that may briefly appear
#   INTRUDER  - should not be on isolated cores
# isolation_monitor.sh parses the report on stdout. Keep its lines unchanged.

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] [duration_sec] [cpu_spec...]

Run a bpftrace sched_switch audit on isolated (or specified) CPU cores,
then classify every observed task as EXPECTED, ALLOWED, or INTRUDER.
bpftrace needs root: when you are not root, the script runs it with sudo.
The report goes to stdout. Status messages go to stderr.

REQUIRED PACKAGES (Debian/Ubuntu):
	bpftrace python3

OPTIONS:
	-h, --help	Show this help message and exit

ARGUMENTS:
	duration_sec	Sampling duration in seconds (default: 5)
	cpu_spec	CPU cores to monitor. Supports:
			  2 7         range min-max (legacy)
			  2-7         range notation
			  2,5,7       comma-separated
			  2-4,7,9-11  mixed ranges and singles
			  2 5 7       space-separated discrete list
			(default: auto-detect from isolcpus)

EXAMPLES:
	${script_name}		# 5s, auto-detect isolated cores
	${script_name} 10		# 10s, auto-detect
	${script_name} 5 2 3	# 5s, force CPUs 2-3 (range)
	${script_name} 5 2-7	# 5s, force CPUs 2-7
	${script_name} 5 2 5 7	# 5s, force CPUs 2,5,7 (discrete)
	${script_name} 5 2-4,7	# 5s, CPUs 2,3,4,7

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
           against the thread comm name with:  [[ "\$comm" =~ \$pattern ]]

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
EOF
}

detect_isolated() {
	local iso
	iso=$(tr -d '\n' </sys/devices/system/cpu/isolated 2>/dev/null)
	[[ -z "$iso" ]] && iso=$(grep -oP 'isolcpus=\K\S+' /proc/cmdline 2>/dev/null)
	echo "$iso"
}

# Expand a cpulist spec (e.g. "2-4,7,9-11") into a sorted, deduplicated
# space-separated list of individual CPU numbers.
# The spec is passed via CPU_SPEC env var (not interpolated into the
# Python source) to avoid code injection from untrusted input.
expand_cpulist() {
	CPU_SPEC="$1" python3 -c "
import os
s=os.environ['CPU_SPEC']; r=set()
for p in s.split(','):
    p=p.strip()
    if not p: continue
    a,_,b=p.partition('-')
    r.update(range(int(a),int(b or a)+1))
print(' '.join(str(x) for x in sorted(r)))
"
}

# A cpulist spec holds CPU numbers and ranges, separated by commas.
is_cpulist() {
	[[ "$1" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]
}

# Load patterns from RC files (skip blank lines and comments)
load_patterns() {
	local file="$1"
	local -n arr=$2
	local line
	if [[ ! -f "$file" ]]; then
		log_warning "Pattern file not found: $file"
		return
	fi
	while IFS= read -r line; do
		[[ -z "$line" || "$line" == \#* ]] && continue
		arr+=("$line")
	done <"$file"
}

classify() {
	local comm="$1" pat
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

# Print the classification report of the bpftrace output in $1 to stdout.
print_report() {
	local raw_log="$1" S="$2"
	local line cpu comm pid cnt verdict total entry
	local -A verdict_counts=([EXPECTED]=0 [ALLOWED]=0 [INTRUDER]=0)
	local intruder_list=()

	printf '\n%s\n  CLASSIFICATION REPORT\n%s\n\n' "$S" "$S"
	printf '  %-12s  %-4s  %-22s  %-8s  %s\n' \
		"VERDICT" "CPU" "COMM" "PID" "SWITCH_COUNT"
	printf '  %s\n' "$(printf '%.0s-' {1..65})"

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
	done <"$raw_log"

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
}

main() {
	local arg duration raw iso cpu_min cpu_max bpf_cpu_filter c joined btrace_prog line
	local cpu_list=() filter_parts=()
	local S="================================================================"

	for arg in "$@"; do
		case "$arg" in
		-h | --help)
			show_help
			exit 0
			;;
		-*)
			log_error "Unknown option: $arg"
			show_help
			exit 1
			;;
		esac
	done

	duration=${1:-5}
	if ! [[ "$duration" =~ ^[0-9]+$ ]] || ((duration < 1)); then
		log_error "duration must be a positive integer (got: '$duration')"
		exit 1
	fi

	# Build cpu_list (space-separated individual CPUs) from arguments
	if [[ -n "${2:-}" ]]; then
		# Join all args after duration with commas, then expand
		raw="${*:2}"
		# Replace spaces with commas so "2 5 7" becomes "2,5,7"
		raw="${raw// /,}"
		if ! is_cpulist "$raw"; then
			log_error "cpu_spec is not a CPU list (got: '${*:2}')"
			show_help
			exit 1
		fi
	else
		raw=""
		iso=$(detect_isolated)
		if [[ -n "$iso" ]] && ! is_cpulist "$iso"; then
			log_error "Cannot read the isolated CPU list '$iso'. Give cpu_spec."
			exit 1
		fi
	fi

	require_commands bpftrace:bpftrace python3:python3 || exit 1

	if [[ -n "$raw" ]]; then
		read -ra cpu_list <<<"$(expand_cpulist "$raw")"
		log_info "Manual override — monitoring CPUs: ${cpu_list[*]}"
	elif [[ -z "$iso" ]]; then
		cpu_list=(2 3)
		log_warning "No isolcpus found — using CPUs: ${cpu_list[*]}"
	else
		read -ra cpu_list <<<"$(expand_cpulist "$iso")"
		log_info "Detected isolated cores: $iso  → CPUs: ${cpu_list[*]}"
	fi

	# Derive min/max for display
	cpu_min=${cpu_list[0]}
	cpu_max=${cpu_list[-1]}

	# Build bpftrace CPU filter expression
	if ((cpu_max - cpu_min + 1 == ${#cpu_list[@]})); then
		# Contiguous range — use efficient >= && <=
		bpf_cpu_filter="cpu >= $cpu_min && cpu <= $cpu_max"
	else
		# Non-contiguous — use explicit || checks
		for c in "${cpu_list[@]}"; do
			filter_parts+=("cpu == $c")
		done
		joined=$(
			IFS='|'
			echo "${filter_parts[*]}"
		)
		bpf_cpu_filter=${joined//|/ || }
	fi

	RAW_LOG=$(mktemp /tmp/sched_audit_XXXXXX.log)
	ERR_LOG=$(mktemp /tmp/sched_audit_XXXXXX.err)
	trap 'rm -f "$RAW_LOG" "$ERR_LOG"' EXIT

	printf '\n%s\n  sched_switch audit  |  cores [%s]  |  %ds sample\n%s\n\n' \
		"$S" "${cpu_list[*]}" "$duration" "$S"

	EXPECTED_PATTERNS=()
	ALLOWED_PATTERNS=()
	load_patterns "${script_folder}/isolation_expected.env" EXPECTED_PATTERNS
	load_patterns "${script_folder}/isolation_allowed.env" ALLOWED_PATTERNS

	if ((${#EXPECTED_PATTERNS[@]} == 0)); then
		log_warning "No EXPECTED patterns loaded — all threads will be INTRUDER or ALLOWED"
	fi
	if ((${#ALLOWED_PATTERNS[@]} == 0)); then
		log_warning "No ALLOWED patterns loaded"
	fi

	btrace_prog='
tracepoint:sched:sched_switch
/ '"$bpf_cpu_filter"' /
{
    @sw[cpu, args->next_comm, args->next_pid] = count();
}

interval:s:'"$duration"'
{
    print(@sw);
    clear(@sw);
    exit();
}
'

	log_info "Running bpftrace for ${duration}s..."
	log_info "(watching CPUs [${cpu_list[*]}] for sched_switch events)"

	if ! as_root bpftrace -e "$btrace_prog" 2>"$ERR_LOG" | tee "$RAW_LOG"; then
		log_error "bpftrace failed:"
		while IFS= read -r line; do
			log_error "  ${line}"
		done <"$ERR_LOG"
		exit 1
	fi
	print_report "$RAW_LOG" "$S"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
