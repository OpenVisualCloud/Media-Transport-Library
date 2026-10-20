#!/bin/bash
# shellcheck disable=SC2317
#
# Runs the KahawaiTest suite on the ports that host validation prepared.
# This script discovers the ports, runs the cases, and reports the results.

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
mtl_folder=$(realpath "${script_folder}/../..")
noctx_folder="${mtl_folder}/tests/integration_tests/noctx"
isolate_sh="${mtl_folder}/tests/tools/isolate/isolate.sh"
declare -A test_cases

# Detect whether to use .local_install (CI) or local build/system paths
if [ -d "${mtl_folder}/.local_install" ]; then
	# CI mode: use .local_install prefix tree. Its binaries find their
	# libraries relative to themselves; no LD_LIBRARY_PATH.
	export PATH="${mtl_folder}/.local_install/dpdk/bin:${mtl_folder}/.local_install/mtl/bin:${PATH}"

	: "${KAHAWAI_TEST_BINARY:="${mtl_folder}/.local_install/mtl/bin/KahawaiTest"}"
else
	# Local mode: use build directory and system-installed libraries
	: "${KAHAWAI_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiTest"}"
fi

# sudo resets PATH even with -E; pass it explicitly via env.
SUDO_PREFIX="sudo -E env PATH=${PATH}"

: "${MAX_RETRIES:=2}"
: "${RETRY_DELAY:=20}"
: "${TMP_FOLDER:=/tmp/mtl_gtest_$(date +%Y%m%d_%H%M%S)_$$}"
: "${LOG_FILE:=${TMP_FOLDER}/gtest.log}"
: "${EXIT_ON_FAILURE:=1}"
: "${NIGHTLY:=1}"                                       # Set to 1 to run full test suite, 0 for quick tests
: "${TEST_CASE_TIMEOUT:=1800}"                          # 30 minutes per test case
: "${NOCTX_CASE_TIMEOUT:=4200}"                         # The noctx case: every NoCtx test, st20p_tx_multithread_stability alone 30 min
: "${HOST_OP_TIMEOUT:=180}"                             # Hard bound for one nicctl listing
: "${TEST_KILL_GRACE:=30}"                              # SIGKILL delay after SIGTERM for a test case
: "${MIN_VFIO_PORTS:=4}"                                # Ports of one PF the suite needs on vfio-pci
: "${GTEST_CASES:=}"                                    # Regex of the case names to run (empty = every case)
: "${SYSFS_PCI_DEVICES:=/sys/bus/pci/devices}"          # Where the ports are looked up; the contract tests point this at a fixture
: "${SYS_HUGEPAGES:=/sys/kernel/mm/hugepages}"          # Where the hugepages are counted; likewise a fixture under test
: "${HOST_FAULT_EXIT:=3}"                               # Exit code meaning "host needs recovery, not a test failure"
: "${TEST_SIP_SEED:=$((RANDOM))}"                       # Seed for the test subnet, so two hosts rarely share one
: "${TEST_P_SIP:="192.168.$((TEST_SIP_SEED % 256)).1"}" # Address of the primary port. Host octet stays in 1..254
# KahawaiTest takes only --p_sip. It counts the host octet up for each further
# port, so the address of the redundant port follows from the primary one. This
# value is reported, not passed.
: "${TEST_R_SIP:="${TEST_P_SIP%.*}.$(((${TEST_P_SIP##*.} % 254) + 1))"}"

if [ "${NIGHTLY}" -eq 0 ]; then
	FAIL_FAST="--gtest_fail_fast" # Skips remaining tests on first failure
else
	FAIL_FAST=""
fi

export KAHAWAI_TEST_BINARY
export MAX_RETRIES
export RETRY_DELAY
export TMP_FOLDER
export LOG_FILE
export EXIT_ON_FAILURE
export NIGHTLY
export TEST_CASE_TIMEOUT
export TEST_SIP_SEED
export TEST_P_SIP
export TEST_R_SIP
export FAIL_FAST

# Signal trap for cleanup on termination
cleanup() {
	trap - SIGINT SIGTERM SIGHUP
	echo "Caught signal, cleaning up..."
	kill_test_processes
	kill -- -$$ 2>/dev/null || true
	exit 130
}
trap cleanup SIGINT SIGTERM SIGHUP

start_time=$(date +%s)
time_taken_by_script() {
	local end_time
	end_time=$(date +%s)
	local elapsed_time=$((end_time - start_time))
	local hours=$((elapsed_time / 3600))
	local minutes=$(((elapsed_time % 3600) / 60))
	local seconds=$((elapsed_time % 60))

	echo "=========================================="
	echo "Time elapsed: ${hours}h ${minutes}m ${seconds}s"
	echo "=========================================="
}

# ── the host ────────────────────────────────────────────────────────────────

dump_driver_state() {
	echo "--- processes ---"
	# pgrep rather than a grep over ps, which would report the greps themselves.
	local pids
	mapfile -t pids < <(pgrep -f 'KahawaiTest|nicctl|devbind' || true)
	if [ "${#pids[@]}" -gt 0 ]; then
		ps -o pid,stat,wchan:32,etimes,args -p "${pids[@]}"
	fi
	# A driver that faulted takes its callers with it, into uninterruptible
	# sleep. Their kernel stacks are what says where it went.
	for pid in $(ps -eo pid=,stat= | awk '$2 ~ /D/ {print $1}'); do
		echo "--- /proc/${pid}/stack ($(ps -o args= -p "${pid}" 2>/dev/null)) ---"
		sudo cat "/proc/${pid}/stack" 2>/dev/null || true
	done
	echo "--- modules ---"
	lsmod | grep -E '^ice|^vfio' || true
	echo "--- dmesg tail ---"
	sudo dmesg -T 2>/dev/null | tail -50 || true
}

# A faulted NIC driver leaves the processes that ask it questions in
# uninterruptible sleep, where not even SIGKILL reclaims them. This script
# cannot fix that, but it must stop waiting: an unbounded wait holds a fleet
# runner for GitHub's 360-minute default.
host_fault() {
	echo "=========================================="
	echo "✗ Host fault: $1"
	echo "This is a host problem, not a test failure. The host needs recovery"
	echo "before it can run tests again, for example:"
	echo "  echo 1 | sudo tee /sys/bus/pci/devices/<pf-bdf>/remove"
	echo "  sleep 1"
	echo "  echo 1 | sudo tee /sys/bus/pci/rescan"
	echo "=========================================="
	dump_driver_state
	kill_test_processes
	time_taken_by_script
	exit "${HOST_FAULT_EXIT}"
}

# nicctl.sh asks the same driver the tests do, so it can block the same way, and
# a timeout here means the host is faulted rather than empty -- so it is recorded
# in a sentinel file that the callers check before reporting "no ports found".
#
# The listing goes to a file rather than up a pipe. timeout signals its own child
# and nothing below it, so a nicctl.sh stuck in a sysfs read leaves that read
# holding the pipe open, and a caller reading it would wait out the whole hang it
# just gave up on.
nicctl_list() {
	local destination=$1
	shift
	local retval=0
	timeout --foreground --signal=SIGTERM --kill-after="${TEST_KILL_GRACE}" \
		"${HOST_OP_TIMEOUT}" "${mtl_folder}/script/nicctl.sh" list "$@" \
		>"${destination}" 2>/dev/null || retval=$?
	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		touch "${TMP_FOLDER}/.nicctl_timeout"
	fi
	return "${retval}"
}

nicctl_wedged() {
	[ -f "${TMP_FOLDER}/.nicctl_timeout" ]
}

not_prepared() {
	echo "✗ $1"
	echo "Host validation did not prepare the host for the suite."
	time_taken_by_script
	exit 1
}

# Prints the vfio-pci ports of the first PF in a nicctl.sh listing that has
# enough of them.
#
# They have to belong to one PF: a transmitter and a receiver on two different
# cards are on two different networks, and every case here expects the pair to
# see each other. A prepared host has six VFs on one PF, but a VF another suite
# left bound is listed too, so group before choosing.
ports_of_one_pf() {
	local listing=$1 port pf group=()
	declare -A by_pf=()
	while read -r port; do
		pf=$(basename "$(readlink -f "${SYSFS_PCI_DEVICES}/${port}/physfn" 2>/dev/null)")
		by_pf["${pf:-standalone}"]+="${port} "
	done < <(awk '$3 == "vfio-pci" {print $2}' "${listing}")

	for pf in $(printf '%s\n' "${!by_pf[@]}" | sort); do
		read -r -a group <<<"${by_pf[$pf]}"
		if [ "${#group[@]}" -ge "${MIN_VFIO_PORTS}" ]; then
			printf '%s\n' "${group[@]}"
			return 0
		fi
	done
	return 1
}

# Prints the NoCtx ports: $1 and $2 of PF A interleaved with two VFs of PF B, as
# TX A, RX B, spare A, spare B. B is a sibling function of A (same bus:device):
# CI cables the two ports of one NIC, never two NICs. VFs of one PF are switched
# inside the NIC and get no RX timestamp, which the strict pacing cases need
# (tests/integration_tests/noctx/README.md). Without a B it prints its
# arguments, the one-PF ports.
noctx_ports() {
	local listing=$1 pf_a port pf group=()
	shift
	declare -A by_pf=()
	pf_a=$(basename "$(readlink -f "${SYSFS_PCI_DEVICES}/$1/physfn" 2>/dev/null)")
	while read -r port; do
		pf=$(basename "$(readlink -f "${SYSFS_PCI_DEVICES}/${port}/physfn" 2>/dev/null)")
		if [ "${pf}" != "${pf_a}" ] && [ "${pf%.*}" = "${pf_a%.*}" ]; then
			by_pf["${pf}"]+="${port} "
		fi
	done < <(awk '$3 == "vfio-pci" {print $2}' "${listing}")

	for pf in $(printf '%s\n' "${!by_pf[@]}" | sort); do
		read -r -a group <<<"${by_pf[$pf]}"
		if [ "${#group[@]}" -ge 2 ]; then
			printf '%s\n' "$1" "${group[0]}" "$2" "${group[1]}"
			return 0
		fi
	done
	printf '%s\n' "$@"
}

nicctl_vf() {
	sudo timeout --foreground --signal=SIGTERM --kill-after="${TEST_KILL_GRACE}" \
		"${HOST_OP_TIMEOUT}" "${mtl_folder}/script/nicctl.sh" "$@"
}

# Removes the VFs that create_vfs made, so the next job finds the host as it was.
remove_vfs() {
	local pf
	for pf in "${created_vf_pfs[@]}"; do
		echo "Removing the VFs of ${pf}"
		nicctl_vf disable_vf "${pf}" || echo "✗ nicctl.sh disable_vf ${pf} failed"
	done
	created_vf_pfs=()
}

# Creates six trusted VFs on an ice PF with link up, and two on another port of
# its card for NoCtx (see noctx_ports). A PF whose card has a second port up
# comes first.
created_vf_pfs=()
create_vfs() {
	local listing="${TMP_FOLDER}/up.listing" pfs=() pf="" sibling="" candidate other

	nicctl_list "${listing}" up || true
	nicctl_wedged && host_fault "nicctl.sh stopped responding while listing NIC ports"
	mapfile -t pfs < <(awk '$3 == "ice" {print $2}' "${listing}")
	pf=${pfs[0]:-}
	for candidate in "${pfs[@]}"; do
		for other in "${pfs[@]}"; do
			if [ "${other}" != "${candidate}" ] && [ "${other%.*}" = "${candidate%.*}" ]; then
				pf=${candidate} sibling=${other}
				break 2
			fi
		done
	done
	[ -n "${pf}" ] || return 0

	trap remove_vfs EXIT
	echo "Creating 6 trusted VFs on ${pf}"
	created_vf_pfs+=("${pf}")
	nicctl_vf create_tvf "${pf}" 6 || echo "✗ nicctl.sh create_tvf ${pf} failed"
	if [ -n "${sibling}" ]; then
		echo "Creating 2 trusted VFs on ${sibling} for NoCtx"
		created_vf_pfs+=("${sibling}")
		nicctl_vf create_tvf "${sibling}" 2 || echo "✗ nicctl.sh create_tvf ${sibling} failed"
	fi
}

# What the suite runs on: four ports of one PF.
discover_ports() {
	local listing="${TMP_FOLDER}/ports.listing" ports=() free_pages

	nicctl_list "${listing}" all || true
	mapfile -t ports < <(ports_of_one_pf "${listing}")
	if [ "${#ports[@]}" -lt "${MIN_VFIO_PORTS}" ]; then
		nicctl_wedged && host_fault "nicctl.sh stopped responding while listing NIC ports"
		create_vfs
		nicctl_list "${listing}" all || true
		mapfile -t ports < <(ports_of_one_pf "${listing}")
	fi
	if [ "${#ports[@]}" -lt "${MIN_VFIO_PORTS}" ]; then
		nicctl_wedged && host_fault "nicctl.sh stopped responding while listing NIC ports"
		cat "${listing}"
		not_prepared "No PF has the ${MIN_VFIO_PORTS} vfio-pci ports the suite needs."
	fi

	# Hugepages are as much a prerequisite as a port: every case is a DPDK
	# process, and one that finds none stops inside EAL on "Cannot get hugepage
	# information" -- a message about DPDK, on a host that has simply been
	# rebooted since it was prepared. Read and named here, reserved by the
	# preparation step, same rule as the ports above.
	free_pages=$(cat "${SYS_HUGEPAGES}"/hugepages-{2048,1048576}kB/free_hugepages 2>/dev/null |
		awk '{sum += $1} END {print sum + 0}')
	if [ "${free_pages}" -eq 0 ]; then
		not_prepared "No free 2 MB or 1 GB hugepages on this host, and every case's EAL needs them."
	fi

	export TEST_PORT_1="${ports[0]}" TEST_PORT_2="${ports[1]}"
	export TEST_PORT_3="${ports[2]}" TEST_PORT_4="${ports[3]}"
	mapfile -t noctx_port < <(noctx_ports "${listing}" "${ports[@]:0:4}")
	# The library probes the DMA devices on vfio-pci on the NUMA node of the
	# ports itself. A host with none runs the DMA cases as skipped.
	export TEST_DMA_ARG="--dma_auto"
}

# ── the cases ───────────────────────────────────────────────────────────────

# One case is the binary with one gtest filter, the ports discovered above, and
# at most a few EAL or pacing options; everything else is the same for all of
# them, so it lives here rather than in twenty copies.
#
# case_env is passed to the env(1) that SUDO_PREFIX already runs the binary
# under, because sudo's environment handling is not this script's to assume.
kahawai_case() {
	local name=$1 filter=$2 case_env=$3
	shift 3
	test_cases["$name"]="${SUDO_PREFIX} ${case_env} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" ${TEST_DMA_ARG} $* ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_${name}.xml --gtest_filter=${filter}"
}

dpdk_case() {
	local name=$1 filter=$2
	shift 2
	kahawai_case "$name" "$filter" '' "$@"
}

generate_test_cases() {
	test_cases=()

	# The baseline suite, always run. NIGHTLY=0 must be a strict subset of
	# NIGHTLY=1, so nothing below this block may be dropped from it.
	#
	# St20_rx and St20_tx are the two longest suites, so each is split over two
	# shards that run as two cases.
	local direction shard
	for direction in rx tx; do
		for shard in 0 1; do
			kahawai_case "st2110_20_${direction}_shard${shard}" "St20_${direction}*" \
				"GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=${shard}"
		done
	done
	dpdk_case st2110_20p 'St20p*'
	dpdk_case st2110_22_rx 'St22_rx*'
	dpdk_case st2110_22_tx 'St22_tx*'
	dpdk_case st2110_22p 'St22p*'
	dpdk_case st2110_3x 'St3*'
	dpdk_case st2110_4x 'St4*'

	if [ "${NIGHTLY}" -ne 1 ]; then
		return
	fi

	# Nightly additions: the suites that are too slow for a pull request, and
	# the transports again under other IOVA, pacing and RSS modes.
	dpdk_case Misc 'Misc*'
	dpdk_case Main 'Main*'
	dpdk_case Sch 'Sch*'
	dpdk_case Cvt 'Cvt*'
	dpdk_case st20s 'St20s*'
	dpdk_case Dma_va 'Dma*' --iova_mode va
	dpdk_case Dma_pa 'Dma*' --iova_mode pa
	dpdk_case digest_1080p_timeout_interval '*digest_1080p_timeout_interval*' \
		--rss_mode l3_l4 --pacing_way tsc --iova_mode pa --multi_src_port
	dpdk_case st20p_auto_pacing_pa 'Main*:St20p*:-*ext*' \
		--rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port
	dpdk_case st20p_auto_pacing_va 'Main*:St20p*:-*ext*' \
		--rss_mode l3_l4 --pacing_way auto --iova_mode va --multi_src_port
	dpdk_case st20p_tsc_pacing 'Main*:St20p*:-*ext*' \
		--rss_mode l3_l4 --pacing_way tsc --iova_mode va --multi_src_port

	# Three that do not fit the shape above: the redundant-path cases take one
	# port list instead of a pair and no DMA, the kernel-socket datapath takes
	# no DPDK port and no root, and noctx is a script of its own because DPDK
	# EAL cannot be re-initialised in one process.
	test_cases[redundant_stats]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\" --auto_start_stop --port_list \"${TEST_PORT_1},${TEST_PORT_2},${TEST_PORT_3},${TEST_PORT_4}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_redundant_stats.xml --gtest_filter=St20p.redundant*:St30p.redundant*:St40p.redundant*"
	test_cases[st20p_kernel_loopback]="\"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\" --auto_start_stop --p_port kernel:lo --r_port kernel:lo ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_kernel_loopback.xml --gtest_filter=St20p*"
	# NOCTX_REQUIRE_STRICT=1: a strict pacing case fails, not skips, on a topology without NIC RX timestamps.
	# MTL_ISOLATE=require: a NOCTX_ISOLATE_CASES case fails, not runs unconfined, without an exclusive CPU partition.
	test_cases[noctx]="TEST_PORT_1=${noctx_port[0]} TEST_PORT_2=${noctx_port[1]} TEST_PORT_3=${noctx_port[2]} TEST_PORT_4=${noctx_port[3]} NOCTX_REQUIRE_STRICT=1 MTL_ISOLATE=require \"${noctx_folder}/run.sh\""
}

# ── running them ────────────────────────────────────────────────────────────

# isolate.sh's verdict for the NoCtx ports, printed before the run.
noctx_isolation_probe() {
	local port_list="${noctx_port[0]},${noctx_port[1]},${noctx_port[2]},${noctx_port[3]}" output
	output=$(sudo env MTL_ISOLATE=require MTL_ISOLATE_PORTS="${port_list}" "${isolate_sh}" -- true 2>&1)
	grep -m 1 'exclusive CPU isolation' <<<"${output}" || echo "no verdict: ${output##*$'\n'}"
}

# Restores the CPU isolation settings and cgroups a SIGKILLed isolate.sh left.
noctx_sweep() {
	sudo "${isolate_sh}" --sweep && return 0
	echo "✗ isolate.sh --sweep failed: CPU isolation settings or cgroups left behind" | sudo tee -a "$LOG_FILE"
	return 1
}

kill_test_processes() {
	# Kill by process group if available
	pkill -SIGKILL -P $$ 2>/dev/null || true
	sudo killall -SIGKILL KahawaiTest 2>/dev/null || true
	sleep 2
}

# These messages suggest configuration errors that require manual intervention
# If those are found in log just give up immediately
declare -a error_messages=(
	"Not a directory"
	"mt_user_params_check, same name for port 1 and 0"
	"EAL: Cannot use IOVA as"
	"libmtl.so: cannot open shared object file:"
	"EAL: Cannot set up DMA remapping, error 12 (Cannot allocate memory)"
	"Error: mt_user_params_check, same name  for port 1 and 0"
	"Error: mt_user_params_check(1), invalid ip 0.0.0.0"
	"cannot open shared object file: No such file or directory"
	"Cannot bind to driver vfio-pci"
)

check_configuration_errors() {
	for i in "${!error_messages[@]}"; do
		if grep -q "${error_messages[$i]}" "$LOG_FILE"; then
			echo "✗ Configuration error detected: ${error_messages[$i]}"
			return 1
		fi
	done
	return 0
}

# The payload's own program, in a variable so that this shell does not expand it:
# $$ is the pid of the new session's leader, and $1..$4 are what setsid passes on.
case_program=$(
	cat <<'PROGRAM'
echo $$ >"$1"
exec timeout --signal=SIGTERM --kill-after="$2" "$3" bash -c "$4"
PROGRAM
)

# Runs one test case bounded by TEST_CASE_TIMEOUT (NOCTX_CASE_TIMEOUT for noctx).
#
# The payload is not piped into `tee`: KahawaiTest runs as root under sudo, so
# an orphan that outlives `timeout` keeps the pipe open and stalls the whole
# pipeline no matter what the timeout did. Output goes to a per-case file that
# `tail -F` streams to the job log instead, and the payload gets its own
# session so every orphan can be reclaimed by session id.
run_case_bounded() {
	local test_name="$1"
	local case_log="${TMP_FOLDER}/${test_name}.out"
	local sid_file="${TMP_FOLDER}/${test_name}.sid"
	local retval=0 limit="${TEST_CASE_TIMEOUT}"

	[ "${test_name}" != noctx ] || limit="${NOCTX_CASE_TIMEOUT}"
	sudo rm -f "${case_log}" "${sid_file}"
	sudo install -m 0600 -o "$(id -u)" /dev/null "${case_log}"

	[ "${test_name}" != noctx ] || noctx_sweep || return 1

	tail -n 0 -F "${case_log}" &
	local tail_pid=$!

	setsid --wait bash -c "${case_program}" gtest-case \
		"${sid_file}" "${TEST_KILL_GRACE}" "${limit}" "${test_cases[$test_name]}" \
		>>"${case_log}" 2>&1 || retval=$?

	kill "${tail_pid}" 2>/dev/null || true
	wait "${tail_pid}" 2>/dev/null || true
	sudo cat "${case_log}" | sudo tee -a "$LOG_FILE" >/dev/null

	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		echo "✗ Test case exceeded ${limit}s: ${test_name}"
		local sid
		sid=$(cat "${sid_file}" 2>/dev/null)
		if [ -n "${sid}" ]; then
			sudo pkill -SIGKILL -s "${sid}" 2>/dev/null || true
		fi
	fi
	if [ "${test_name}" = noctx ] && ! noctx_sweep && [ "${retval}" -eq 0 ]; then
		retval=1
	fi

	return "${retval}"
}

# A retry runs the same case on the same ports. It deliberately does not touch
# the driver or the VFs in between: a case that only passes after its NIC has
# been rebuilt underneath it is not a pass worth reporting, and the rebuild is
# how the runner used to get wedged.
run_test_with_retry() {
	local test_name="$1"
	local attempt=1

	echo "=========================================="
	echo "Running: $test_name" | sudo tee -a "$LOG_FILE"
	echo "Command: ${test_cases[$test_name]}" | sudo tee -a "$LOG_FILE"
	echo "=========================================="

	while [ $attempt -le "$MAX_RETRIES" ]; do
		echo "Attempt $attempt/$MAX_RETRIES for: $test_name"

		RETVAL=0
		run_case_bounded "$test_name" || RETVAL=$?
		if [[ $RETVAL == 0 ]]; then
			echo "✓ Test passed: $test_name" | sudo tee -a "$LOG_FILE"
			return 0
		elif (! check_configuration_errors); then
			echo "✗ Test failed due to configuration errors: $test_name (attempt $attempt/$MAX_RETRIES)" | sudo tee -a "$LOG_FILE"
			return 2
		else
			echo "✗ Attempt failed for $test_name (attempt $attempt/$MAX_RETRIES)" | sudo tee -a "$LOG_FILE"

			kill_test_processes

			if [ $attempt -lt "$MAX_RETRIES" ]; then
				echo "Waiting $RETRY_DELAY seconds before retry..."
				sleep "$RETRY_DELAY"
				((attempt++))
			else
				break
			fi
		fi
	done

	echo "✗ Test failed after $MAX_RETRIES attempts: $test_name" | sudo tee -a "$LOG_FILE"

	if [ "$EXIT_ON_FAILURE" -eq 1 ]; then
		echo "Exiting due to test failure."
		kill_test_processes
		time_taken_by_script
		exit 1
	fi
	return 1
}

print_configuration() {
	echo "=========================================="
	echo "Configuration:"
	echo "=========================================="
	echo "KAHAWAI_TEST_BINARY: $KAHAWAI_TEST_BINARY"
	echo "MAX_RETRIES: $MAX_RETRIES"
	echo "RETRY_DELAY: $RETRY_DELAY seconds"
	echo "TMP_FOLDER: $TMP_FOLDER"
	echo "LOG_FILE: $LOG_FILE"
	echo "EXIT_ON_FAILURE: $EXIT_ON_FAILURE"
	echo "NIGHTLY: $NIGHTLY"
	echo "TEST_CASE_TIMEOUT: $TEST_CASE_TIMEOUT seconds"
	echo "NOCTX_CASE_TIMEOUT: $NOCTX_CASE_TIMEOUT seconds"
	echo "HOST_OP_TIMEOUT: $HOST_OP_TIMEOUT seconds"
	echo "TEST_KILL_GRACE: $TEST_KILL_GRACE seconds"
	echo "TEST_SIP_SEED: $TEST_SIP_SEED"
	echo "TEST_P_SIP: $TEST_P_SIP"
	echo "TEST_R_SIP: $TEST_R_SIP"
	echo "FAIL_FAST: ${FAIL_FAST:-<not set>}"
	echo "TEST_PORT_1..4: ${TEST_PORT_1} ${TEST_PORT_2} ${TEST_PORT_3} ${TEST_PORT_4}"
	echo "NoCtx ports: ${noctx_port[*]}"
	echo "NoCtx CPU isolation: ${noctx_isolation:-<not probed, no noctx case>}"
	echo "DMA: ${TEST_DMA_ARG}"
	echo "=========================================="
	echo ""
}

# Lets tests source the helpers above without running the suite.
if [ -n "${GTEST_SH_SOURCE_ONLY:-}" ]; then
	return 0
fi

sudo mkdir -p "${TMP_FOLDER}" 2>/dev/null
if [ ! -d "${TMP_FOLDER}" ]; then
	echo "Error: Could not create temporary folder at ${TMP_FOLDER}"
	exit 1
fi

echo "Starting MTL test suite..."
kill_test_processes
discover_ports
generate_test_cases
[ -z "${test_cases[noctx]+set}" ] || noctx_isolation=$(noctx_isolation_probe)
print_configuration

for test_name in "${!test_cases[@]}"; do
	[[ -z ${GTEST_CASES} || $test_name =~ ${GTEST_CASES} ]] || continue
	echo "$test_name" "${test_cases[$test_name]}"
	if ! run_test_with_retry "$test_name"; then
		retval=$?
		if [ $retval -eq 2 ]; then
			echo "✗ Test aborted due to configuration errors: $test_name"
		fi
		kill_test_processes
		time_taken_by_script
		exit 1
	fi
done

kill_test_processes

print_configuration

# Generate final summary from complete log
echo ""
echo "=========================================="
echo "FINAL TEST RESULTS SUMMARY"
echo "=========================================="

declare -a failed_all
declare -a passed_all
declare -a failed_catastrophically
declare -a unstable

mapfile -t failed_all < <(grep "\[  FAILED  \]" "$LOG_FILE" | grep -v "listed below:" | awk '{print $4}' | sort -u)
mapfile -t passed_all < <(grep "\[       OK \]" "$LOG_FILE" | grep -v "listed below:" | awk '{print $4}' | sort -u)

# Identify unstable/flaky tests (both passed and failed during retries)
for test in "${failed_all[@]}"; do
	if printf '%s\n' "${passed_all[@]}" | grep -Fxq "$test"; then
		unstable+=("$test")
	else
		failed_catastrophically+=("$test")
	fi
done

passed_count=${#passed_all[@]}
unstable_count=${#unstable[@]}
critical_count=${#failed_catastrophically[@]}
total_tests=$((passed_count + critical_count))

if [ "$total_tests" -gt 0 ]; then
	pass_rate=$(awk "BEGIN {printf \"%.2f\", ($passed_count * 100 / $total_tests)}")
else
	pass_rate="0.00"
fi

printf "%-20s: %d\n" "Passed tests" "$passed_count"
printf "%-20s: %d\n" "Failed tests" "$critical_count"
printf "%-20s: %d\n" "Unstable (flaky)" "$unstable_count"
printf "%-20s: %d\n" "Total tests" "$total_tests"
printf "%-20s: %s%%\n" "Pass rate" "$pass_rate"

if [ "$unstable_count" -gt 0 ]; then
	echo ""
	echo "⚠ Unstable/Flaky tests detected (failed then passed on retry):"
	for test in "${unstable[@]}"; do
		echo "  - $test"
	done
fi

if [ "$critical_count" -gt 0 ]; then
	echo ""
	echo "✗ Failed tests (never passed):"
	for test in "${failed_catastrophically[@]}"; do
		echo "  - $test"
	done
fi

echo "=========================================="

time_taken_by_script

if [ "$critical_count" -gt 0 ]; then
	exit 1
fi
