#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Continuous isolation monitoring with spike analysis.
#
# Runs isolation_report.sh in a tight loop (default 30s samples),
# logs every iteration with timestamps, and on Ctrl+C prints a
# spike analysis showing when the worst isolation violations occurred.

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] [sample_duration] [cpu_spec...]

Run isolation_report.sh in a loop and keep the report of each sample in a
log directory. Press Ctrl+C to stop. The script then prints a spike analysis
that shows when the worst isolation violations occurred. bpftrace needs root:
when you are not root, isolation_report.sh runs it with sudo.

REQUIRED PACKAGES (Debian/Ubuntu):
	bpftrace python3

OPTIONS:
	-h, --help	Show this help message and exit

ARGUMENTS:
	sample_duration	Duration of one sample in seconds (default: 30)
	cpu_spec	CPU cores to monitor, as isolation_report.sh reads them
			(default: auto-detect from isolcpus)

EXAMPLES:
	${script_name}		# 30s samples, auto-detect cores
	${script_name} 3		# 3s samples
	${script_name} 5 2-7	# 5s samples, CPUs 2-7
	${script_name} 5 2 5 7	# 5s samples, CPUs 2, 5, 7
	${script_name} 5 2-4,7	# 5s samples, CPUs 2,3,4,7
EOF
}

# ── cleanup & analysis on exit ──
finish() {
	local rc=$?
	printf '\n\n'
	local S="================================================================"

	# Use actual completed samples, not ITERATION (which may have been
	# incremented before the last sample finished).
	local completed=${#TIMESTAMPS[@]}

	if ((completed == 0)); then
		log_warning "No samples collected."
		rm -rf "$LOG_DIR"
		exit "$rc"
	fi

	printf '\n%s\n  SPIKE ANALYSIS  (%d samples collected)\n%s\n\n' "$S" "$completed" "$S"

	# CSV header
	printf '%-6s  %-24s  %10s  %10s  %10s  %10s\n' \
		"#" "TIMESTAMP" "TOTAL_SW" "EXPECTED" "ALLOWED" "INTRUDER"
	printf '  %s\n' "$(printf '%.0s-' {1..80})"

	local max_intruder=0 max_intruder_idx=0
	local max_total=0 max_total_idx=0
	local i

	for ((i = 0; i < completed; i++)); do
		printf '%-6d  %-24s  %10d  %10d  %10d  %10d' \
			"$((i + 1))" "${TIMESTAMPS[$i]}" "${TOTALS[$i]}" \
			"${EXPECTED_COUNTS[$i]}" "${ALLOWED_COUNTS[$i]}" "${INTRUDERS[$i]}"

		# track worst intruder spike
		if ((INTRUDERS[i] > max_intruder)); then
			max_intruder=${INTRUDERS[$i]}
			max_intruder_idx=$i
		fi
		# track worst total spike
		if ((TOTALS[i] > max_total)); then
			max_total=${TOTALS[$i]}
			max_total_idx=$i
		fi

		# mark spikes inline
		if ((INTRUDERS[i] > 0)); then
			printf '  ← INTRUDERS!'
		fi
		printf '\n'
	done

	printf '\n%s\n  WORST SPIKES\n%s\n\n' "$S" "$S"

	printf '  Biggest TOTAL switch spike:\n'
	printf '    Sample #%d  |  %s  |  %d total switches\n\n' \
		"$((max_total_idx + 1))" "${TIMESTAMPS[$max_total_idx]}" "$max_total"

	printf '  Biggest INTRUDER spike:\n'
	if ((max_intruder > 0)); then
		printf '    Sample #%d  |  %s  |  %d intruder switches\n' \
			"$((max_intruder_idx + 1))" "${TIMESTAMPS[$max_intruder_idx]}" "$max_intruder"
		if [[ -n "${INTRUDER_DETAILS[$max_intruder_idx]:-}" ]]; then
			printf '    Intruders: %s\n' "${INTRUDER_DETAILS[$max_intruder_idx]}"
		fi
	else
		printf '    None — no intruders observed across all samples!\n'
	fi

	# compute averages
	local sum_total=0 sum_intruder=0 sum_allowed=0
	for ((i = 0; i < completed; i++)); do
		sum_total=$((sum_total + TOTALS[i]))
		sum_intruder=$((sum_intruder + INTRUDERS[i]))
		sum_allowed=$((sum_allowed + ALLOWED_COUNTS[i]))
	done

	printf '\n%s\n  AVERAGES (per %ds sample)\n%s\n\n' "$S" "$SAMPLE_DURATION" "$S"
	printf '  Avg total switches  : %d\n' "$((sum_total / completed))"
	printf '  Avg allowed switches: %d\n' "$((sum_allowed / completed))"
	printf '  Avg intruder switches: %d\n' "$((sum_intruder / completed))"

	# rename max spike files with _MAX suffix
	local src dst prev
	if ((max_intruder > 0)); then
		src="${LOG_FILES[$max_intruder_idx]}"
		dst="${src%.log}_MAX_INTRUDER.log"
		mv "$src" "$dst" 2>/dev/null && printf '\n  Renamed worst intruder log → %s\n' "$(basename "$dst")"
	fi
	if ((max_total > 0)); then
		src="${LOG_FILES[$max_total_idx]}"
		if [[ -f "$src" ]]; then
			dst="${src%.log}_MAX_TOTAL.log"
			mv "$src" "$dst" 2>/dev/null && printf '  Renamed worst total log   → %s\n' "$(basename "$dst")"
		else
			# already renamed as intruder max — add total tag too
			prev="${src%.log}_MAX_INTRUDER.log"
			dst="${src%.log}_MAX_INTRUDER_MAX_TOTAL.log"
			mv "$prev" "$dst" 2>/dev/null && printf '  Renamed worst total log   → %s\n' "$(basename "$dst")"
		fi
	fi

	printf '\n  Full logs saved in: %s/\n\n' "$LOG_DIR"
	printf '%s\n  Done — %d samples over %s\n%s\n\n' \
		"$S" "$completed" \
		"$(date -u -d @$((completed * SAMPLE_DURATION)) +%H:%M:%S)" \
		"$S"
}

# ── parse one report output, extract counts ──
# Sets PARSED_TOTAL, PARSED_EXPECTED, PARSED_ALLOWED, PARSED_INTRUDER and
# PARSED_INTRUDER_DETAILS. Call it in the current shell, not in $(...): a
# subshell loses the values.
parse_report() {
	local log_file="$1" line name

	PARSED_TOTAL=0
	PARSED_EXPECTED=0
	PARSED_ALLOWED=0
	PARSED_INTRUDER=0
	PARSED_INTRUDER_DETAILS=""

	while IFS= read -r line; do
		if [[ "$line" =~ Total\ switches\ observed\ :\ ([0-9]+) ]]; then
			PARSED_TOTAL="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ EXPECTED\ switches.*:\ ([0-9]+) ]]; then
			PARSED_EXPECTED="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ ALLOWED.*switches.*:\ ([0-9]+) ]]; then
			PARSED_ALLOWED="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ INTRUDER\ switches.*:\ ([0-9]+) ]]; then
			PARSED_INTRUDER="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ ^[[:space:]]+X[[:space:]]+(.+) ]]; then
			name="${BASH_REMATCH[1]}"
			if [[ -n "$PARSED_INTRUDER_DETAILS" ]]; then
				PARSED_INTRUDER_DETAILS+="; ${name}"
			else
				PARSED_INTRUDER_DETAILS="${name}"
			fi
		fi
	done <"$log_file"
}

main() {
	local arg ts ts_file log_file
	local report_script="${script_folder}/isolation_report.sh"

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

	SAMPLE_DURATION=${1:-30}
	CPU_ARGS=("${@:2}")

	# ── validate inputs ──
	if ! [[ "$SAMPLE_DURATION" =~ ^[0-9]+$ ]] || ((SAMPLE_DURATION < 1)); then
		log_error "sample duration must be a positive integer (got: '$SAMPLE_DURATION')"
		exit 1
	fi

	if [[ ! -x "$report_script" ]]; then
		log_error "cannot find or execute $report_script"
		exit 1
	fi
	require_commands bpftrace:bpftrace python3:python3 || exit 1

	# ── state ──
	ITERATION=0
	TIMESTAMPS=()
	TOTALS=()
	INTRUDERS=()
	ALLOWED_COUNTS=()
	EXPECTED_COUNTS=()
	INTRUDER_DETAILS=()
	LOG_FILES=()
	LOG_DIR=$(mktemp -d /tmp/isolation_monitor_XXXXXX)
	trap finish EXIT

	log_info "Continuous Isolation Monitor"
	log_info "Sample duration : ${SAMPLE_DURATION}s"
	log_info "CPU args        : ${CPU_ARGS[*]:-auto-detect}"
	log_info "Log directory   : ${LOG_DIR}/"
	log_info "Press Ctrl+C to stop and see spike analysis"

	# ── main loop ──
	while true; do
		ITERATION=$((ITERATION + 1))
		ts=$(date '+%Y-%m-%d %H:%M:%S')
		ts_file=$(date '+%Y-%m-%d_%H-%M-%S')
		log_file="${LOG_DIR}/${ts_file}.log"

		printf '\n──── Sample #%d  |  %s ────\n' "$ITERATION" "$ts"

		# run the report, capture output
		"$report_script" "$SAMPLE_DURATION" "${CPU_ARGS[@]}" 2>&1 | tee "$log_file"

		# parse results
		parse_report "$log_file"

		TIMESTAMPS+=("$ts")
		TOTALS+=("$PARSED_TOTAL")
		EXPECTED_COUNTS+=("$PARSED_EXPECTED")
		ALLOWED_COUNTS+=("$PARSED_ALLOWED")
		INTRUDERS+=("$PARSED_INTRUDER")
		INTRUDER_DETAILS+=("$PARSED_INTRUDER_DETAILS")
		LOG_FILES+=("$log_file")

		# quick inline status
		printf '  → Sample #%d: total=%d expected=%d allowed=%d intruder=%d\n' \
			"$ITERATION" "$PARSED_TOTAL" "$PARSED_EXPECTED" "$PARSED_ALLOWED" "$PARSED_INTRUDER"

		# no sleep — loop immediately for minimal downtime
	done
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
