#!/bin/bash
# isolation_monitor.sh — continuous isolation monitoring with spike analysis
#
# Runs isolation_report.sh in a tight loop (default 30s samples),
# logs every iteration with timestamps, and on Ctrl+C prints a
# spike analysis showing when the worst isolation violations occurred.
#
# Usage:
#   sudo ./isolation_monitor.sh [sample_duration] [cpu_min] [cpu_max]
#   sudo ./isolation_monitor.sh              # 5s samples, auto-detect cores
#   sudo ./isolation_monitor.sh 3            # 3s samples
#   sudo ./isolation_monitor.sh 5 2 7        # 5s samples, CPUs 2-7

set -euo pipefail

SAMPLE_DURATION=${1:-30}
CPU_ARGS=("${@:2}")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT_SCRIPT="${SCRIPT_DIR}/isolation_report.sh"
LOG_DIR="/tmp/isolation_monitor_$$"
SUMMARY_CSV="${LOG_DIR}/summary.csv"

mkdir -p "$LOG_DIR"

# ── state ──
ITERATION=0
declare -a TIMESTAMPS=()
declare -a TOTALS=()
declare -a INTRUDERS=()
declare -a ALLOWED_COUNTS=()
declare -a EXPECTED_COUNTS=()
declare -a INTRUDER_DETAILS=()
declare -a LOG_FILES=()

# ── cleanup & analysis on exit ──
finish() {
	echo ""
	echo ""
	local S="================================================================"

	# Use actual completed samples, not ITERATION (which may have been
	# incremented before the last sample finished).
	local completed=${#TIMESTAMPS[@]}

	if ((completed == 0)); then
		echo "No samples collected."
		rm -rf "$LOG_DIR"
		exit 0
	fi

	printf '\n%s\n  SPIKE ANALYSIS  (%d samples collected)\n%s\n\n' "$S" "$completed" "$S"

	# CSV header
	printf '%-6s  %-24s  %10s  %10s  %10s  %10s\n' \
		"#" "TIMESTAMP" "TOTAL_SW" "EXPECTED" "ALLOWED" "INTRUDER"
	printf '  %s\n' "$(printf '%.0s-' {1..80})"

	local max_intruder=0 max_intruder_idx=0
	local max_total=0 max_total_idx=0

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
	if ((max_intruder > 0)); then
		local src="${LOG_FILES[$max_intruder_idx]}"
		local dst="${src%.log}_MAX_INTRUDER.log"
		mv "$src" "$dst" 2>/dev/null && printf '\n  Renamed worst intruder log → %s\n' "$(basename "$dst")"
	fi
	if ((max_total > 0)); then
		local src="${LOG_FILES[$max_total_idx]}"
		if [[ -f "$src" ]]; then
			local dst="${src%.log}_MAX_TOTAL.log"
			mv "$src" "$dst" 2>/dev/null && printf '  Renamed worst total log   → %s\n' "$(basename "$dst")"
		else
			# already renamed as intruder max — add total tag too
			local prev="${src%.log}_MAX_INTRUDER.log"
			local dst="${src%.log}_MAX_INTRUDER_MAX_TOTAL.log"
			mv "$prev" "$dst" 2>/dev/null && printf '  Renamed worst total log   → %s\n' "$(basename "$dst")"
		fi
	fi

	printf '\n  Full logs saved in: %s/\n\n' "$LOG_DIR"
	printf '%s\n  Done — %d samples over %s\n%s\n\n' \
		"$S" "$completed" \
		"$(date -u -d @$((completed * SAMPLE_DURATION)) +%H:%M:%S)" \
		"$S"
}

trap finish EXIT

# ── parse one report output, extract counts ──
parse_report() {
	local log_file="$1"
	local total=0 expected=0 allowed=0 intruder=0
	local intruder_names=""

	while IFS= read -r line; do
		if [[ "$line" =~ Total\ switches\ observed\ :\ ([0-9]+) ]]; then
			total="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ EXPECTED\ switches.*:\ ([0-9]+) ]]; then
			expected="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ ALLOWED.*switches.*:\ ([0-9]+) ]]; then
			allowed="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ INTRUDER\ switches.*:\ ([0-9]+) ]]; then
			intruder="${BASH_REMATCH[1]}"
		elif [[ "$line" =~ ^[[:space:]]+X[[:space:]]+(.+) ]]; then
			local name="${BASH_REMATCH[1]}"
			if [[ -n "$intruder_names" ]]; then
				intruder_names+="; ${name}"
			else
				intruder_names="${name}"
			fi
		fi
	done <"$log_file"

	echo "${total} ${expected} ${allowed} ${intruder}"
	# stash intruder details via global
	_PARSED_INTRUDER_DETAILS="$intruder_names"
}

# ── validate inputs ──
if ! [[ "$SAMPLE_DURATION" =~ ^[0-9]+$ ]] || ((SAMPLE_DURATION < 1)); then
	echo "Error: sample duration must be a positive integer (got: '$SAMPLE_DURATION')" >&2
	exit 1
fi

if [[ ! -x "$REPORT_SCRIPT" ]]; then
	echo "Error: cannot find or execute $REPORT_SCRIPT" >&2
	exit 1
fi

echo "================================================================"
echo "  Continuous Isolation Monitor"
echo "  Sample duration : ${SAMPLE_DURATION}s"
echo "  CPU args        : ${CPU_ARGS[*]:-auto-detect}"
echo "  Log directory   : ${LOG_DIR}/"
echo "  Press Ctrl+C to stop and see spike analysis"
echo "================================================================"
echo ""

# ── main loop ──
while true; do
	ITERATION=$((ITERATION + 1))
	ts=$(date '+%Y-%m-%d %H:%M:%S')
	ts_file=$(date '+%Y-%m-%d_%H-%M-%S')
	log_file="${LOG_DIR}/${ts_file}.log"

	printf '\n──── Sample #%d  |  %s ────\n' "$ITERATION" "$ts"

	# run the report, capture output
	"$REPORT_SCRIPT" "$SAMPLE_DURATION" "${CPU_ARGS[@]}" 2>&1 | tee "$log_file"

	# parse results
	_PARSED_INTRUDER_DETAILS=""
	read -r total expected allowed intruder <<<"$(parse_report "$log_file")"

	TIMESTAMPS+=("$ts")
	TOTALS+=("${total:-0}")
	EXPECTED_COUNTS+=("${expected:-0}")
	ALLOWED_COUNTS+=("${allowed:-0}")
	INTRUDERS+=("${intruder:-0}")
	INTRUDER_DETAILS+=("$_PARSED_INTRUDER_DETAILS")
	LOG_FILES+=("$log_file")

	# quick inline status
	printf '  → Sample #%d: total=%d expected=%d allowed=%d intruder=%d\n' \
		"$ITERATION" "${total:-0}" "${expected:-0}" "${allowed:-0}" "${intruder:-0}"

	# no sleep — loop immediately for minimal downtime
done
