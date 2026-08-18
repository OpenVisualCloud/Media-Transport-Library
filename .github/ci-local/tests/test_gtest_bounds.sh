#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Contract tests for the bounded-execution helpers in .github/scripts/gtest.sh.
# They exist because the failure they guard against is invisible in a passing
# run: an unbounded modprobe or a test-case pipeline held open by an orphan
# does not fail, it hangs, and holds a bare-metal runner for hours.

set -uo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
gtest_sh="${root_dir}/.github/scripts/gtest.sh"
work_dir=$(mktemp -d)
failures=0

cleanup_work_dir() {
	rm -rf "${work_dir}"
}
trap cleanup_work_dir EXIT

pass() {
	echo "ok   - $1"
}

fail() {
	echo "FAIL - $1" >&2
	failures=$((failures + 1))
}

check() {
	local description=$1 expected=$2 actual=$3
	if [ "${expected}" = "${actual}" ]; then
		pass "${description}"
	else
		fail "${description} (expected '${expected}', got '${actual}')"
	fi
}

# nicctl.sh, modprobe and sudo are stubbed through PATH. sudo has to be a real
# executable rather than a shell function: the helpers run it under `timeout`,
# which execs a program and never sees a function.
export PATH="${work_dir}/bin:${PATH}"
mkdir -p "${work_dir}/bin"

{
	echo '#!/usr/bin/env bash'
	echo 'while [[ ${1:-} == -* ]]; do shift; done'
	echo 'exec "$@"'
} >"${work_dir}/bin/sudo"
chmod +x "${work_dir}/bin/sudo"

stub_nicctl_ports() {
	local count=$1
	{
		echo '#!/usr/bin/env bash'
		echo 'if [ "${1}" != "list" ]; then exit 0; fi'
		echo "for i in \$(seq 1 ${count}); do echo \"ice\${i} 0000:00:0\${i}.0 vfio-pci 0\"; done"
	} >"${work_dir}/nicctl.sh"
	chmod +x "${work_dir}/nicctl.sh"
}

stub_lsmod() {
	local modules=$1
	{
		echo '#!/usr/bin/env bash'
		printf 'printf "%%s\\n" %s\n' "${modules}"
	} >"${work_dir}/bin/lsmod"
	chmod +x "${work_dir}/bin/lsmod"
}

stub_modprobe() {
	{
		echo '#!/usr/bin/env bash'
		echo "printf '%s\\n' \"\$*\" >>'${work_dir}/modprobe.calls'"
	} >"${work_dir}/bin/modprobe"
	chmod +x "${work_dir}/bin/modprobe"
	: >"${work_dir}/modprobe.calls"
}

export GTEST_SH_SOURCE_ONLY=1
export TMP_FOLDER="${work_dir}/tmp"
export LOG_FILE="${work_dir}/tmp/gtest.log"
export HOST_OP_TIMEOUT=2
export TEST_KILL_GRACE=1
export TEST_CASE_TIMEOUT=2
mkdir -p "${TMP_FOLDER}"
: >"${LOG_FILE}"

# shellcheck source=/dev/null
source "${gtest_sh}"
# gtest.sh installs its own signal handlers and defines its own cleanup(); the
# test owns process teardown from here on.
trap - SIGINT SIGTERM SIGHUP
trap cleanup_work_dir EXIT
# The suite itself must not have run.
check 'sourcing gtest.sh does not run the suite' '0' "$(find "${TMP_FOLDER}" -name 'gtest_*.xml' | wc -l)"

# mtl_folder resolves to the real repo; point nicctl.sh at the stub instead.
# shellcheck disable=SC2034 # read by the sourced gtest.sh helpers
mtl_folder="${work_dir}"

# 1. A command that finishes inside the bound reports its own status.
retval=0
run_bounded 'fast command' true || retval=$?
check 'run_bounded passes a fast command through' '0' "${retval}"

retval=0
run_bounded 'failing command' false || retval=$?
check 'run_bounded preserves a real failure' '1' "${retval}"

# 2. A command that outlives the bound is a host fault, not a test failure, and
#    it must not be retried. host_fault exits and reclaims the caller's children,
#    so it runs in its own process.
start=$(date +%s)
retval=0
bash -c 'source "$1"; run_bounded "wedged command" sleep 30' _ "${gtest_sh}" \
	>"${work_dir}/fault.log" 2>&1 || retval=$?
elapsed=$(($(date +%s) - start))
check 'a wedged host command exits with HOST_FAULT_EXIT' '3' "${retval}"
if [ "${elapsed}" -lt 10 ]; then
	pass "a wedged host command gives up quickly (${elapsed}s)"
else
	fail "a wedged host command gives up quickly (took ${elapsed}s)"
fi
if grep -q 'Host fault' "${work_dir}/fault.log"; then
	pass 'the host fault is reported as a host problem'
else
	fail 'the host fault is reported as a host problem'
fi

# 3. nicctl.sh hanging is recorded, so callers report a host fault instead of
#    a misleading "no ports found".
rm -f "${TMP_FOLDER}/.nicctl_timeout"
{
	echo '#!/usr/bin/env bash'
	echo 'sleep 30'
} >"${work_dir}/nicctl.sh"
chmod +x "${work_dir}/nicctl.sh"
mkdir -p "${work_dir}/script"
ln -sf "${work_dir}/nicctl.sh" "${work_dir}/script/nicctl.sh"
nicctl_list all >/dev/null 2>&1 || true
if nicctl_wedged; then
	pass 'a hanging nicctl.sh is recorded as wedged'
else
	fail 'a hanging nicctl.sh is recorded as wedged'
fi

# 4. A host that already has the ports the suite needs must not be reloaded:
#    every ICE reload is another chance to hit the probe fault.
rm -f "${TMP_FOLDER}/.nicctl_timeout"
stub_nicctl_ports 4
ln -sf "${work_dir}/nicctl.sh" "${work_dir}/script/nicctl.sh"
stub_lsmod 'ice vfio_pci'
stub_modprobe
reset_ice_driver >/dev/null 2>&1
check 'a usable ICE state skips the reload' '0' "$(wc -l <"${work_dir}/modprobe.calls")"

# 5. Too few bound ports means the driver really is reloaded.
stub_nicctl_ports 1
ln -sf "${work_dir}/nicctl.sh" "${work_dir}/script/nicctl.sh"
stub_modprobe
reset_ice_driver >/dev/null 2>&1
check 'an unusable ICE state reloads the driver' '2' "$(wc -l <"${work_dir}/modprobe.calls")"

# 6. A test case that leaves an orphan holding its stdout must still be
#    reclaimed at the bound. Before the fix the orphan kept the `tee` pipe open
#    and the step ran until the job timeout.
declare -A test_cases=()
# shellcheck disable=SC2034 # read by run_case_bounded in the sourced gtest.sh
test_cases['orphan']="echo case-started; sleep 300 & sleep 300"
start=$(date +%s)
retval=0
run_case_bounded 'orphan' >"${work_dir}/case.log" 2>&1 || retval=$?
elapsed=$(($(date +%s) - start))
check 'an orphaned test case is reported as timed out' '124' "${retval}"
if [ "${elapsed}" -lt 20 ]; then
	pass "an orphaned test case does not stall the run (${elapsed}s)"
else
	fail "an orphaned test case does not stall the run (took ${elapsed}s)"
fi
if grep -q 'case-started' "${LOG_FILE}"; then
	pass 'test case output still reaches the run log'
else
	fail 'test case output still reaches the run log'
fi
if pgrep -f 'sleep 300' >/dev/null 2>&1; then
	fail 'the orphaned payload is killed by session id'
	pkill -f 'sleep 300' || true
else
	pass 'the orphaned payload is killed by session id'
fi

if [ "${failures}" -ne 0 ]; then
	echo "gtest.sh bounded-execution contracts: ${failures} FAILED" >&2
	exit 1
fi
echo 'gtest.sh bounded-execution contracts: PASS'
