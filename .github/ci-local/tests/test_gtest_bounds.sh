#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Contract tests for the two halves of a gtest job: .github/scripts/ci/
# bind-test-ports.sh, which prepares the NIC, and .github/scripts/gtest.sh,
# which only reads it and runs the cases.
#
# They exist because the failures they guard against are invisible in a passing
# run. An unbounded NIC operation does not fail, it hangs, and holds a bare-metal
# runner for hours; a test-case pipeline held open by an orphan does the same; a
# lost shard index runs the same half of the suite twice and reports success; and
# a transmitter and a receiver picked from two different cards are on two
# different networks.

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

gtest_in_process() {
	# gtest.sh in a process of its own, for the paths that end in exit. Sourcing
	# it resolves mtl_folder to the real repository and puts a restored DPDK tree
	# in front of PATH, so both are pointed back at the fixture afterwards.
	bash -c 'source "$1"; mtl_folder="$2"; export PATH="$3"; eval "$4"' \
		_ "${gtest_sh}" "${work_dir}" "${work_dir}/bin:${PATH}" "$1"
}

check() {
	local description=$1 expected=$2 actual=$3
	if [ "${expected}" = "${actual}" ]; then
		pass "${description}"
	else
		fail "${description} (expected '${expected}', got '${actual}')"
	fi
}

# The host commands are stubbed through PATH. sudo has to be a real executable
# rather than a shell function: the scripts run it under `timeout`, which execs a
# program and never sees a function.
export PATH="${work_dir}/bin:${PATH}"
mkdir -p "${work_dir}/bin"

cat >"${work_dir}/bin/sudo" <<'SUDO'
#!/usr/bin/env bash
while [[ ${1:-} == -* ]]; do shift; done
exec "$@"
SUDO
chmod +x "${work_dir}/bin/sudo"

stub_command() {
	local name=$1
	shift
	{
		echo '#!/usr/bin/env bash'
		printf '%s\n' "$@"
	} >"${work_dir}/bin/${name}"
	chmod +x "${work_dir}/bin/${name}"
}

# ── the ports of one PF ─────────────────────────────────────────────────────
# A fixture PCI tree: two cards with four vfio-pci VFs each, so that "four ports"
# and "four ports of one card" are different answers and only the second is
# right. The cards are on different NUMA nodes and only one of them has two DMA
# channels beside it, so taking the first card listed is not the answer either --
# and the card that must win sorts first, as an E830 at 15:xx does against an
# E810 at c9:xx.
export SYSFS_PCI_DEVICES="${work_dir}/sys/bus/pci/devices"
listed_first_pf=0000:c9:00.0
listed_first_numa=0
wanted_pf=0000:15:00.0
wanted_numa=1
ports_listing=""
mkdir -p "${work_dir}/sys/bus/pci/drivers/vfio-pci"
row=0
for pf in "${listed_first_pf}:${listed_first_numa}" "${wanted_pf}:${wanted_numa}"; do
	numa=${pf##*:}
	pf=${pf%:*}
	mkdir -p "${SYSFS_PCI_DEVICES}/${pf}"
	ports_listing+="${row}	${pf}	ice	${numa}	42	eth${row}"$'\n'
	row=$((row + 1))
	for index in 1 2 3 4; do
		vf="${pf%.*}.${index}"
		mkdir -p "${SYSFS_PCI_DEVICES}/${vf}"
		ln -sfn "../${pf}" "${SYSFS_PCI_DEVICES}/${vf}/physfn"
		ln -sfn "../../drivers/vfio-pci" "${SYSFS_PCI_DEVICES}/${vf}/driver"
		ln -sfn "../${vf}" "${SYSFS_PCI_DEVICES}/${pf}/virtfn${index}"
		ports_listing+="${row}	${vf}	vfio-pci	${numa}	42	N/A"$'\n'
		row=$((row + 1))
	done
done

stub_nicctl() {
	# A nicctl.sh that lists the fixture tree, and records what it was asked to
	# do to it.
	mkdir -p "${work_dir}/script"
	cat >"${work_dir}/script/nicctl.sh" <<SH
#!/usr/bin/env bash
printf '%s\n' "\$*" >>${work_dir}/nicctl.calls
if [ "\${1}" != "list" ]; then exit 0; fi
cat ${work_dir}/ports.listing
SH
	chmod +x "${work_dir}/script/nicctl.sh"
	: >"${work_dir}/nicctl.calls"
}

stub_hanging_nicctl() {
	mkdir -p "${work_dir}/script"
	cat >"${work_dir}/script/nicctl.sh" <<'SH'
#!/usr/bin/env bash
sleep 30
SH
	chmod +x "${work_dir}/script/nicctl.sh"
}

printf '%s' "${ports_listing}" >"${work_dir}/ports.listing"

# Two DMA channels beside the card that must win, and a single one on the other
# node, which is one short of what a test case takes.
{
	echo 'DMA devices using DPDK-compatible driver'
	echo '0000:e7:01.0 '\''Device 0b25'\'' drv=vfio-pci unused=idxd numa_node=1'
	echo '0000:e7:01.1 '\''Device 0b25'\'' drv=vfio-pci unused=idxd numa_node=1'
	echo ''
	echo 'DMA devices using kernel driver'
	echo '0000:6a:01.0 '\''Device 0b25'\'' drv=idxd unused=vfio-pci numa_node=0'
} >"${work_dir}/dma.listing"
stub_command dpdk-devbind.py \
	"printf '%s\n' \"\$*\" >>${work_dir}/devbind.calls" \
	"cat ${work_dir}/dma.listing"
: >"${work_dir}/devbind.calls"

# ── gtest.sh, sourced ───────────────────────────────────────────────────────
export GTEST_SH_SOURCE_ONLY=1
export TMP_FOLDER="${work_dir}/tmp"
export LOG_FILE="${work_dir}/tmp/gtest.log"
# The hugepage pool is a host fact like the PCI tree, so it is a fixture too:
# these tests have to answer the same on a prepared test host and on the
# GitHub-hosted runner that verifies them, which has no hugepages at all. The
# case that asks what an unprepared host is told points this at its own file.
export PROC_MEMINFO="${work_dir}/proc_meminfo"
printf 'MemFree: 8 kB\nHugePages_Total: 2048\nHugePages_Free: 2048\n' >"${PROC_MEMINFO}"
export HOST_OP_TIMEOUT=2
export TEST_KILL_GRACE=1
export TEST_CASE_TIMEOUT=2
mkdir -p "${TMP_FOLDER}"
: >"${LOG_FILE}"

# shellcheck source=/dev/null
source "${gtest_sh}"
# gtest.sh installs its own signal handlers and defines its own cleanup(); the
# test owns process teardown from here on. It also puts a restored DPDK tree in
# front of PATH, and this host's real devices are not the fixture, so the stubs
# go back in front of it.
trap - SIGINT SIGTERM SIGHUP
trap cleanup_work_dir EXIT
export PATH="${work_dir}/bin:${PATH}"
# The suite itself must not have run.
check 'sourcing gtest.sh does not run the suite' '0' "$(find "${TMP_FOLDER}" -name 'gtest_*.xml' | wc -l)"

# mtl_folder resolves to the real repo; point nicctl.sh at the stub instead.
# shellcheck disable=SC2034 # read by the sourced gtest.sh helpers
mtl_folder="${work_dir}"
stub_nicctl

# 1. The suite runs on one card. Both fixture cards have enough ports, so the
#    four chosen must still all belong to the same one.
discover_ports >"${work_dir}/discover.log" 2>&1
selected_pfs=$(for port in "${TEST_PORT_1}" "${TEST_PORT_2}" "${TEST_PORT_3}" "${TEST_PORT_4}"; do
	basename "$(readlink -f "${SYSFS_PCI_DEVICES}/${port}/physfn")"
done | sort -u)
check 'the four ports all belong to one PF' "${wanted_pf}" "${selected_pfs}"
check 'the ports are the DMA channels'\'' own NUMA node' \
	'0000:e7:01.0 0000:e7:01.1' "${TEST_DMA_PORT_P} ${TEST_DMA_PORT_R}"

# 2. A PF with too few bound ports is not a candidate, however many ports the
#    host has in total.
saved_listing=$(cat "${work_dir}/ports.listing")
# Six ports, three on each card: enough ports, and not enough on either card.
awk 'NR <= 3 || (NR >= 5 && NR <= 7)' "${work_dir}/ports.listing" >"${work_dir}/short.listing"
mv "${work_dir}/short.listing" "${work_dir}/ports.listing"
retval=0
gtest_in_process discover_ports >"${work_dir}/short.log" 2>&1 || retval=$?
check 'a card with too few ports is refused' '1' "${retval}"
if grep -q 'sudo task ci:bind-test-ports' "${work_dir}/short.log"; then
	pass 'the refusal names the command that prepares the host'
else
	fail 'the refusal names the command that prepares the host'
fi
printf '%s\n' "${saved_listing}" >"${work_dir}/ports.listing"

# 3. nicctl.sh hanging is a host fault, not "no ports found": the driver has
#    stopped answering, and nothing this script does next can change that.
#    host_fault exits and reclaims the caller's children, so it runs in its own
#    process.
rm -f "${TMP_FOLDER}/.nicctl_timeout"
stub_hanging_nicctl
start=$(date +%s)
retval=0
gtest_in_process discover_ports >"${work_dir}/fault.log" 2>&1 || retval=$?
elapsed=$(($(date +%s) - start))
check 'a wedged nicctl.sh exits with HOST_FAULT_EXIT' '3' "${retval}"
if [ "${elapsed}" -lt 10 ]; then
	pass "a wedged nicctl.sh gives up quickly (${elapsed}s)"
else
	fail "a wedged nicctl.sh gives up quickly (took ${elapsed}s)"
fi
if grep -q 'Host fault' "${work_dir}/fault.log"; then
	pass 'the host fault is reported as a host problem'
else
	fail 'the host fault is reported as a host problem'
fi
stub_nicctl

# 4. gtest.sh prepares nothing. Its whole job is to read the state the CI side
#    left; a create_tvf or a bind from here would rebuild the card under a suite
#    that is already running.
discover_ports >/dev/null 2>&1
generate_test_cases
check 'gtest.sh asks nicctl.sh for listings only' '0' \
	"$(grep -cv '^list ' "${work_dir}/nicctl.calls")"
check 'gtest.sh binds nothing' '0' "$(grep -c 'bind' "${work_dir}/devbind.calls")"

# 4b. A host that serves no DMA channel runs the suite without DMA rather than
#     not at all: --dma_dev is what makes the library look for one, and the
#     cases that need it skip themselves when the list is empty. Refusing here
#     costs the leg every case that has nothing to do with DMA -- which is what
#     it did, on every fleet host, for three rounds.
saved_dma_listing=$(cat "${work_dir}/dma.listing")
dma_case=$(
	cat <<'CASES'
discover_ports >/dev/null; generate_test_cases; printf '%s\n' "${test_cases[Dma_va]}"
CASES
)
: >"${work_dir}/dma.listing"
retval=0
out=$(gtest_in_process "${dma_case}" 2>&1) || retval=$?
check 'a host with no DMA channel still runs the suite' '0' "${retval}"
if grep -q -- '--dma_dev' <<<"${out}"; then
	fail 'a case on a host without channels is given no --dma_dev'
else
	pass 'a case on a host without channels is given no --dma_dev'
fi
# One channel is a working DMA setup, not half of one: a DSA device carries
# several queues and MTL enumerates every dmadev the device exposes.
printf '%s\n' "${saved_dma_listing}" | head -n 2 >"${work_dir}/dma.listing"
out=$(gtest_in_process "${dma_case}" 2>&1)
check 'the one channel a host has is given to the cases' '--dma_dev "0000:e7:01.0"' \
	"$(grep -o -- '--dma_dev "[^"]*"' <<<"${out}")"
printf '%s\n' "${saved_dma_listing}" >"${work_dir}/dma.listing"
out=$(gtest_in_process "${dma_case}" 2>&1)
check 'both channels of a prepared host are given to the cases' \
	'--dma_dev "0000:e7:01.0,0000:e7:01.1"' \
	"$(grep -o -- '--dma_dev "[^"]*"' <<<"${out}")"

# 4b-ii. A channel on another NUMA node is worse than no channel at all. The
#     library grants a session a channel of the port's own socket and no other
#     (mt_dma_request_dev), while the suite's st_test_dma_available only counts
#     the ones that registered -- so a case handed a foreign channel neither
#     skips itself nor offloads, it runs the offload path's expectations against
#     a plain memcpy. That is how an E810 host with its card on NUMA 2 failed
#     digest_ooo_slice_4320p with 143 incomplete frames against a limit of 16.
{
	echo 'DMA devices using DPDK-compatible driver'
	echo '0000:6a:01.0 '\''Device 0b25'\'' drv=vfio-pci unused=idxd numa_node=0'
	echo '0000:6a:01.1 '\''Device 0b25'\'' drv=vfio-pci unused=idxd numa_node=0'
} >"${work_dir}/dma.listing"
foreign_case=$(
	cat <<'CASES'
discover_ports; printf 'ARG=[%s]\n' "${TEST_DMA_ARG}"
CASES
)
out=$(gtest_in_process "${foreign_case}" 2>&1)
check 'a channel on another NUMA node is not served to the cases' 'ARG=[]' \
	"$(grep -o 'ARG=\[.*\]' <<<"${out}")"
if grep -q "not on NUMA node ${wanted_numa}" <<<"${out}"; then
	pass 'the log says the channels are on the wrong node, not that there are none'
else
	fail 'the log says the channels are on the wrong node, not that there are none'
fi
printf '%s\n' "${saved_dma_listing}" >"${work_dir}/dma.listing"

# 4c. Hugepages are as much a prerequisite as a port, and a host that has been
#     rebooted since it was prepared has none: every case then stops inside EAL
#     on "Cannot get hugepage information", which reads as a broken build. This
#     script reserves nothing -- it says which command does.
printf 'MemFree: 1 kB\nHugePages_Total: 0\nHugePages_Free: 0\n' >"${work_dir}/meminfo"
retval=0
out=$(PROC_MEMINFO="${work_dir}/meminfo" gtest_in_process 'discover_ports' 2>&1) || retval=$?
check 'a host with no hugepages is refused' '1' "${retval}"
if grep -q 'hugepage' <<<"${out}" && grep -q 'sudo task ci:bind-test-ports' <<<"${out}"; then
	pass 'the refusal names the command that reserves them'
else
	fail 'the refusal names the command that reserves them'
fi

# 5. The two long suites are sharded, and each shard must run its own half. A
#    lost index is silent: both shards run everything and the leg still passes.
shards=$(for name in "${!test_cases[@]}"; do
	case "${test_cases[$name]}" in
	*GTEST_SHARD_INDEX*) printf '%s\n' "${test_cases[$name]}" | grep -o 'GTEST_SHARD_INDEX=[0-9]*' ;;
	esac
done | sort -u | wc -l)
check 'each shard carries its own index' '2' "${shards}"

# 6. NIGHTLY=0 is the pull-request gate. Anything it runs that the nightly does
#    not is a case that no nightly report ever covers.
case_names=$(
	cat <<'CASES'
discover_ports >/dev/null; generate_test_cases; printf "%s\n" "${!test_cases[@]}"
CASES
)
baseline=$(NIGHTLY=0 gtest_in_process "${case_names}" | sort)
nightly=$(NIGHTLY=1 gtest_in_process "${case_names}" | sort)
check 'the baseline suite is a subset of the nightly one' '' \
	"$(comm -23 <(printf '%s\n' "${baseline}") <(printf '%s\n' "${nightly}") | tr '\n' ' ' | sed 's/ *$//')"
if [ "$(printf '%s\n' "${nightly}" | wc -l)" -gt "$(printf '%s\n' "${baseline}" | wc -l)" ]; then
	pass 'the nightly suite adds cases to the baseline'
else
	fail 'the nightly suite adds cases to the baseline'
fi

# 7. A test case that leaves an orphan holding its stdout must still be
#    reclaimed at the bound. Before the fix the orphan kept the `tee` pipe open
#    and the step ran until the job timeout.
test_cases=()
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

# ── bind-test-ports.sh, the half that does prepare the NIC ──────────────────
# Run out of a fixture tree, because the script resolves the repository from its
# own location and calls the nicctl.sh it finds there.
bind_root="${work_dir}/repo"
mkdir -p "${bind_root}/.github/scripts/ci" "${bind_root}/script"
cp "${root_dir}/.github/scripts/ci/bind-test-ports.sh" "${bind_root}/.github/scripts/ci/"
ln -sfn "${work_dir}/script/nicctl.sh" "${bind_root}/script/nicctl.sh"
bind_ports="${bind_root}/.github/scripts/ci/bind-test-ports.sh"

# Extra VAR=VALUE arguments are passed through to the script's environment, for
# the cases that ask what it does under MTL_CI_REQUIRE_DMA.
run_bind() {
	: >"${work_dir}/modprobe.calls"
	env HOST_OP_TIMEOUT=2 SYSFS_PCI_DEVICES="${SYSFS_PCI_DEVICES}" \
		SYSFS_VFIO_DENYLIST="${work_dir}/vfio.denylist" \
		SYSFS_HUGEPAGES="${work_dir}/nr_hugepages" \
		"$@" bash "${bind_ports}" >"${work_dir}/bind.log" 2>&1
}

stub_command id 'echo 0'
stub_command lsmod "printf '%s\n' Module ice vfio_pci"
stub_command modprobe "printf '%s\n' \"\$*\" >>${work_dir}/modprobe.calls"
: >"${work_dir}/modprobe.calls"
# A host whose vfio-pci already allows Intel DSA, and whose hugepages are
# reserved. The cases that need the other answer write these two files.
echo Y >"${work_dir}/vfio.denylist"
echo 4096 >"${work_dir}/nr_hugepages"

# The channels as this script finds them, before anything has bound them: two
# free ones beside the card that must win, and one that idxd holds on the other
# node -- which it must leave exactly where it is.
free_dma_listing() {
	{
		echo 'Other DMA devices'
		echo '0000:e7:01.0 '\''Device 0b25'\'' unused=idxd,vfio-pci numa_node=1'
		echo '0000:e7:01.1 '\''Device 0b25'\'' unused=idxd,vfio-pci numa_node=1'
		echo ''
		echo 'DMA devices using kernel driver'
		echo '0000:6a:01.0 '\''Device 0b25'\'' drv=idxd unused=vfio-pci numa_node=0'
	} >"${work_dir}/dma.listing"
}

# 8. It creates the VFs on the card whose NUMA node has the DMA channels, and
#    binds those channels. Both are what gtest.sh then finds by reading.
free_dma_listing
: >"${work_dir}/nicctl.calls"
: >"${work_dir}/devbind.calls"
retval=0
run_bind || retval=$?
check 'preparing a healthy host succeeds' '0' "${retval}"
check 'it prepares the card that has DMA channels on its own node' \
	"create_tvf ${wanted_pf} 6" "$(grep '^create_tvf' "${work_dir}/nicctl.calls")"
if grep -q -- '-b vfio-pci 0000:e7:01.0' "${work_dir}/devbind.calls" &&
	grep -q -- '-b vfio-pci 0000:e7:01.1' "${work_dir}/devbind.calls"; then
	pass 'it binds the free DMA channels of the ports'\'' own NUMA node'
else
	fail 'it binds the free DMA channels of the ports'\'' own NUMA node'
fi
# It takes no channel it does not need: two are enough, and a third device --
# here one a kernel driver holds on the other node -- is left where it is.
if grep -q -- '-b vfio-pci 0000:6a:01.0' "${work_dir}/devbind.calls"; then
	fail 'a channel it does not need is left alone'
else
	pass 'a channel it does not need is left alone'
fi
if grep -q 'sudo reboot' "${work_dir}/bind.log"; then
	fail 'preparing a healthy host asks for no reboot'
else
	pass 'preparing a healthy host asks for no reboot'
fi

# 8b. A host whose only channels are held by a kernel driver is prepared, not
#     refused. Taking Intel DSA from idxd is what dpdk-devbind.py does, and it
#     is the whole difference between a gtest leg that runs and one that stops
#     at this step -- the DMA cases of the suite skip themselves when there is
#     no channel, so nothing here is worth failing a leg over.
{
	echo 'DMA devices using kernel driver'
	echo '0000:e7:01.0 '\''Device 0b25'\'' drv=idxd unused=vfio-pci numa_node=1'
	echo '0000:e7:01.1 '\''Device 0b25'\'' drv=idxd unused=vfio-pci numa_node=1'
	echo '0000:6a:01.0 '\''Device 0b25'\'' drv=idxd unused=vfio-pci numa_node=0'
} >"${work_dir}/dma.listing"
: >"${work_dir}/devbind.calls"
retval=0
run_bind || retval=$?
check 'a host whose channels are all held by idxd is prepared' '0' "${retval}"
if grep -q -- '-b vfio-pci 0000:e7:01.0' "${work_dir}/devbind.calls" &&
	grep -q -- '-b vfio-pci 0000:e7:01.1' "${work_dir}/devbind.calls"; then
	pass 'it takes the channels of the ports'\'' own node from the kernel driver'
else
	fail 'it takes the channels of the ports'\'' own node from the kernel driver'
fi

# 8c. vfio-pci will not probe Intel DSA (8086:0b25) while its denylist is on,
#     and the parameter is read-only once the module is loaded. Reloading it is
#     a runtime act, so the job does it instead of asking for a reboot.
echo N >"${work_dir}/vfio.denylist"
: >"${work_dir}/devbind.calls"
retval=0
run_bind || retval=$?
check 'a host whose vfio-pci denylists DSA is still prepared' '0' "${retval}"
if grep -q -- '^-r vfio-pci$' "${work_dir}/modprobe.calls" &&
	grep -q -- '^vfio-pci disable_denylist=1$' "${work_dir}/modprobe.calls"; then
	pass 'it reloads vfio-pci with the denylist off'
else
	fail 'it reloads vfio-pci with the denylist off'
fi
echo Y >"${work_dir}/vfio.denylist"
retval=0
run_bind || retval=$?
if grep -q -- '^-r vfio-pci$' "${work_dir}/modprobe.calls"; then
	fail 'a host whose vfio-pci already allows DSA is not reloaded'
else
	pass 'a host whose vfio-pci already allows DSA is not reloaded'
fi

# 8d. A host with no DMA device at all still runs the suite: every case that
#     uses DMA asks the library for a channel and skips itself when there is
#     none, so the leg reports the tests it can run instead of nothing. A host
#     that is meant to have channels says so with MTL_CI_REQUIRE_DMA=1.
: >"${work_dir}/dma.listing"
: >"${work_dir}/devbind.calls"
retval=0
run_bind || retval=$?
check 'a host with no DMA device runs the suite anyway' '0' "${retval}"
if grep -q 'without DMA' "${work_dir}/bind.log"; then
	pass 'the run says which cases the missing channels cost it'
else
	fail 'the run says which cases the missing channels cost it'
fi
if grep -q -- '-b vfio-pci' "${work_dir}/devbind.calls"; then
	fail 'it binds nothing when there is nothing to bind'
else
	pass 'it binds nothing when there is nothing to bind'
fi
retval=0
run_bind MTL_CI_REQUIRE_DMA=1 || retval=$?
check 'a host that must serve DMA is refused when it does not' '1' "${retval}"
if grep -q 'doc/dma.md' "${work_dir}/bind.log"; then
	pass 'the refusal names where serving a channel is written down'
else
	fail 'the refusal names where serving a channel is written down'
fi

# 8d-ii. A host whose every channel sits on a node no NIC is on serves none of
#     them. The library pairs a session with a channel of the port's own socket
#     and no other, so binding one from elsewhere would not give the suite DMA --
#     it would give its DMA cases a channel they are counted as having and never
#     granted, which is a failing case where a skipped one belongs.
{
	echo 'Other DMA devices'
	echo '0000:c1:01.0 '\''Device 0b25'\'' unused=idxd,vfio-pci numa_node=2'
	echo '0000:c1:01.1 '\''Device 0b25'\'' unused=idxd,vfio-pci numa_node=2'
} >"${work_dir}/dma.listing"
: >"${work_dir}/devbind.calls"
retval=0
run_bind || retval=$?
check 'a host whose channels are all on another node is prepared anyway' '0' "${retval}"
if grep -q -- '-b vfio-pci' "${work_dir}/devbind.calls"; then
	fail 'it binds no channel the library could not hand to a session'
else
	pass 'it binds no channel the library could not hand to a session'
fi
if grep -q "own socket" "${work_dir}/bind.log"; then
	pass 'the run says a channel on another node does not count'
else
	fail 'the run says a channel on another node does not count'
fi
free_dma_listing

# 8e. Hugepages are the other thing every DPDK process here needs and nothing
#     in the job reserved: without them EAL stops on "Cannot get hugepage
#     information", which reads like a broken build rather than a host that was
#     rebooted. A host that has enough is left alone -- a host may reserve more
#     than this suite needs, and lowering it would take them from whatever
#     asked for them.
echo 0 >"${work_dir}/nr_hugepages"
retval=0
run_bind MIN_HUGEPAGES=1024 || retval=$?
check 'preparing a host with no hugepages succeeds' '0' "${retval}"
check 'it reserves the hugepages the suite needs' '1024' "$(cat "${work_dir}/nr_hugepages")"
echo 4096 >"${work_dir}/nr_hugepages"
run_bind MIN_HUGEPAGES=1024 || true
check 'it leaves a host that reserved more alone' '4096' "$(cat "${work_dir}/nr_hugepages")"

# 9. A NIC operation that never returns is a host fault here too, and with the
#    same exit code: the job must stop, not wait out its 45-minute bound.
stub_hanging_nicctl
start=$(date +%s)
retval=0
run_bind || retval=$?
elapsed=$(($(date +%s) - start))
check 'a wedged NIC operation exits with HOST_FAULT_EXIT' '3' "${retval}"
if [ "${elapsed}" -lt 10 ]; then
	pass "preparation gives up on a wedged card quickly (${elapsed}s)"
else
	fail "preparation gives up on a wedged card quickly (took ${elapsed}s)"
fi
if grep -q '/sys/bus/pci/rescan' "${work_dir}/bind.log"; then
	pass 'the host fault names how to recover the card'
else
	fail 'the host fault names how to recover the card'
fi
stub_nicctl

# 10. Every refusal names the command that fixes it, because the person reading
#     it is looking at a job log and not at this script.
stub_command lsmod "printf '%s\n' Module vfio_pci"
retval=0
run_bind || retval=$?
check 'a host without ice is refused' '1' "${retval}"
if grep -q 'sudo task ci:activate-ice' "${work_dir}/bind.log"; then
	pass 'the refusal names how to load the driver'
else
	fail 'the refusal names how to load the driver'
fi
stub_command lsmod "printf '%s\n' Module ice vfio_pci"

stub_command id 'echo 1000'
retval=0
run_bind || retval=$?
check 'preparing the NIC as a plain user is refused' '1' "${retval}"
if grep -q 'sudo task ci:bind-test-ports' "${work_dir}/bind.log"; then
	pass 'the refusal names how to run it with the rights it needs'
else
	fail 'the refusal names how to run it with the rights it needs'
fi

if [ "${failures}" -ne 0 ]; then
	echo "gtest host contracts: ${failures} FAILED" >&2
	exit 1
fi
echo 'gtest host contracts: PASS'
