#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# ST2110 functionality tests — KahawaiTest (integration gtest) on real VFs.
#
# One binary, one mtl_init() for the whole run. Needs two DPDK-bound VFs and
# root. Records the DPDK version in-run (tasks.md RULE: a run that cannot name
# what it loaded proves nothing).
#
# Usage:
#   sudo ./st2110-test/run-kahawai.sh [P_PORT] [R_PORT] [-- <extra gtest args>]
#
# Env (override defaults):
#   P_PORT   TX VF BDF            (default 0000:af:01.0)
#   R_PORT   RX VF BDF            (default 0000:af:01.1)
#   FILTER   gtest filter         (default St20p*:St30p*:St40p*:Main.*)
#   PACING   --pacing_way value   (unset = hardware auto; try 'tsc' or 'rl')
#   LEVEL    gtest level          (default: CI-mandatory; set 'all' for the rest)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

P_PORT="${1:-${P_PORT:-0000:af:01.0}}"
R_PORT="${2:-${R_PORT:-0000:af:01.1}}"
FILTER="${FILTER:-St20p*:St30p*:St40p*:Main.*}"
BIN="$REPO/build/tests/KahawaiTest"

# Everything after `--` is passed straight to KahawaiTest.
EXTRA=()
while [ $# -gt 0 ]; do
	[ "$1" = "--" ] && {
		shift
		EXTRA=("$@")
		break
	}
	shift
done

[ -x "$BIN" ] || {
	echo "ERROR: $BIN not built. Run ./build.sh first." >&2
	exit 1
}
[ "$(id -u)" -eq 0 ] || echo "WARN: not root — VF access will fail." >&2

CMD=("$BIN" --auto_start_stop --p_port "$P_PORT" --r_port "$R_PORT"
	--log_level notice --gtest_filter="$FILTER")
[ -n "${PACING:-}" ] && CMD+=(--pacing_way "$PACING")
[ -n "${LEVEL:-}" ] && CMD+=(--level "$LEVEL")
[ "${#EXTRA[@]}" -gt 0 ] && CMD+=("${EXTRA[@]}")

echo "== ST2110 functionality (KahawaiTest) =="
echo "   P_PORT=$P_PORT  R_PORT=$R_PORT  filter=$FILTER  pacing=${PACING:-auto}"
echo "+ ${CMD[*]}"
echo

# Tee so we can prove the loaded DPDK version after the run.
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
"${CMD[@]}" 2>&1 | tee "$LOG"
rc="${PIPESTATUS[0]}"

echo
echo "-- version proof --"
grep -i "dpdk version:" "$LOG" || echo "WARN: no 'dpdk version:' line — rerun with --log_level notice"
echo "-- exit code: $rc --"
exit "$rc"
