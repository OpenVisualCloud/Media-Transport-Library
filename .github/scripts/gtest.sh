#!/bin/bash
# shellcheck disable=SC2317
#
# Runs the KahawaiTest suite on a host that is already prepared for it: the ICE
# driver loaded by `sudo task ci:activate-ice`, and the ports created by
# `sudo task ci:bind-test-ports`.
#
# This script changes no host state. It finds the ports, runs the cases and
# reports. A port that is missing is reported with the command that creates it
# and never created here: bringing a driver or a set of VFs back underneath a
# suite that is already running is how a bare-metal runner ends up wedged for
# hours, and preparing the host is the CI/CD side's job, not the tests'.

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
mtl_folder=$(realpath "${script_folder}/../..")
declare -A test_cases

# Detect whether to use .local_install (CI) or local build/system paths
if [ -d "${mtl_folder}/.local_install" ]; then
	# CI mode: use .local_install prefix tree
	export PATH="${mtl_folder}/.local_install/dpdk/bin:${mtl_folder}/.local_install/mtl/bin:${PATH}"
	export LD_LIBRARY_PATH="${mtl_folder}/.local_install/mtl/lib/x86_64-linux-gnu:${mtl_folder}/.local_install/dpdk/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

	: "${KAHAWAI_TEST_BINARY:="${mtl_folder}/.local_install/mtl/bin/KahawaiTest"}"
else
	# Local mode: use build directory and system-installed libraries
	: "${KAHAWAI_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiTest"}"
fi

# sudo strips LD_LIBRARY_PATH even with -E; pass it explicitly via env. A tree
# with no .local_install never set it, and a caller running under `set -u` is
# entitled to source this script without tripping over that.
SUDO_PREFIX="sudo -E env LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} PATH=${PATH}"

: "${MAX_RETRIES:=2}"
: "${RETRY_DELAY:=20}"
: "${TMP_FOLDER:=/tmp/mtl_gtest_$(date +%Y%m%d_%H%M%S)_$$}"
: "${LOG_FILE:=${TMP_FOLDER}/gtest.log}"
: "${EXIT_ON_FAILURE:=1}"
: "${NIGHTLY:=1}"                                                                    # Set to 1 to run full test suite, 0 for quick tests
: "${TEST_CASE_TIMEOUT:=1800}"                                                       # 30 minutes per test case
: "${HOST_OP_TIMEOUT:=180}"                                                          # Hard bound for one nicctl listing
: "${TEST_KILL_GRACE:=30}"                                                           # SIGKILL delay after SIGTERM for a test case
: "${MIN_VFIO_PORTS:=4}"                                                             # Ports of one PF the suite needs on vfio-pci
: "${DMA_CHANNELS:=2}"                                                               # DMA channels a case is given when the host serves them
: "${SYSFS_PCI_DEVICES:=/sys/bus/pci/devices}"                                       # Where the ports are looked up; the contract tests point this at a fixture
: "${PROC_MEMINFO:=/proc/meminfo}"                                                   # Where the hugepages are counted; likewise a fixture under test
: "${HOST_FAULT_EXIT:=3}"                                                            # Exit code meaning "host needs recovery, not a test failure"
: "${TEST_SIP_SEED:=$((RANDOM))}"                                                    # Seed for generating TEST_P_SIP when not provided
: "${TEST_P_SIP:="192.168.$((TEST_SIP_SEED % 256)).$((TEST_SIP_SEED % 256))"}"       # Primary test IP for gtest
: "${TEST_R_SIP:="192.168.$((TEST_SIP_SEED % 256)).$(((TEST_SIP_SEED + 1) % 256))"}" # Remote test IP for gtest

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
	echo "The host is not prepared for the suite. Prepare it with:"
	echo "  sudo task ci:activate-ice       # E8xx cards, which need the Kahawai driver"
	echo "  sudo task ci:bind-test-ports"
	time_taken_by_script
	exit 1
}

# The DMA channels a case can be given: bound to vfio-pci, and on one NUMA node
# when a node is named.
dma_channels() {
	dpdk-devbind.py --status-dev dma |
		awk -v want="${1:-}" '$1 !~ /^[0-9a-f]+:[0-9a-f]+:[0-9a-f]+\.[0-9a-f]+$/ {next}
			/drv=vfio-pci/ && (want == "" || $0 ~ ("numa_node=" want)) {print $1}'
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

# What the suite runs on: four ports of one PF, and two DMA channels beside
# them. Reading, only -- see the header.
discover_ports() {
	local listing="${TMP_FOLDER}/ports.listing" ports=() channels=() numa dma_list free_pages

	nicctl_list "${listing}" all || true
	mapfile -t ports < <(ports_of_one_pf "${listing}")
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
	free_pages=$(awk '/^HugePages_Free:/ {print $2}' "${PROC_MEMINFO}")
	if [ "${free_pages:-0}" -eq 0 ]; then
		not_prepared "No free 2 MB hugepages on this host, and every case's EAL needs them."
	fi

	# The DMA channels of the ports' own NUMA node, and only those. The library
	# pairs a session with a channel of the port's socket and no other
	# (mt_dma_request_dev, doc/dma.md 3.4), so a channel on another node
	# registers, is counted by st_test_dma_available -- and is then never handed
	# to a session. Passing one makes every DMA case run without the offload it
	# is testing instead of skipping: on an E810 host whose card sits on NUMA 2
	# with the channels on 0 and 1, that turned digest_ooo_slice_4320p into 143
	# incomplete frames against a limit of 16.
	numa=$(awk -v port="${ports[0]}" '$2 == port {print $4}' "${listing}")
	mapfile -t channels < <(dma_channels "${numa}")

	# A host with no channel of its own runs the suite without DMA offload rather
	# than not at all. Every case that copies with DMA asks the library for a
	# channel first (st_test_dma_available) and reports itself skipped when there
	# is none, so refusing here would cost the leg every case that has nothing to
	# do with DMA -- and serving a channel is the preparation step's job
	# (`sudo task ci:bind-test-ports`), not this script's.
	if [ "${#channels[@]}" -eq 0 ]; then
		if [ "$(dma_channels | wc -l)" -gt 0 ]; then
			echo "This host's DMA channels are not on NUMA node ${numa}, which is where the ports"
			echo "are, so none of them can be used here."
		else
			echo "No DMA channel on vfio-pci: 'sudo task ci:bind-test-ports' serves them."
		fi
		echo "The suite runs without DMA offload, and its DMA cases will report themselves"
		echo "skipped."
	fi

	# Exported for the noctx tests, which take the four ports from the
	# environment and run one process per case.
	export TEST_PORT_1="${ports[0]}" TEST_PORT_2="${ports[1]}"
	export TEST_PORT_3="${ports[2]}" TEST_PORT_4="${ports[3]}"
	export TEST_DMA_PORT_P="${channels[0]:-}" TEST_DMA_PORT_R="${channels[1]:-}"
	# What every case is given, built once: the option with the channels this
	# host serves, and nothing at all when it serves none -- an empty --dma_dev
	# is a device named "" for the library to look for and not find.
	if [ "${#channels[@]}" -gt 0 ]; then
		dma_list=${channels[*]}
		export TEST_DMA_ARG="--dma_dev \"${dma_list// /,}\""
	else
		export TEST_DMA_ARG=""
	fi
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
	test_cases[noctx]="\"${mtl_folder}/tests/integration_tests/noctx/run.sh\""
}

# ── running them ────────────────────────────────────────────────────────────

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

# Runs one test case bounded by TEST_CASE_TIMEOUT.
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
	local retval=0

	sudo rm -f "${case_log}" "${sid_file}"
	sudo install -m 0666 /dev/null "${case_log}"

	tail -n 0 -F "${case_log}" &
	local tail_pid=$!

	setsid --wait bash -c "${case_program}" gtest-case \
		"${sid_file}" "${TEST_KILL_GRACE}" "${TEST_CASE_TIMEOUT}" "${test_cases[$test_name]}" \
		>>"${case_log}" 2>&1 || retval=$?

	kill "${tail_pid}" 2>/dev/null || true
	wait "${tail_pid}" 2>/dev/null || true
	sudo cat "${case_log}" | sudo tee -a "$LOG_FILE" >/dev/null

	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		echo "✗ Test case exceeded ${TEST_CASE_TIMEOUT}s: ${test_name}"
		local sid
		sid=$(cat "${sid_file}" 2>/dev/null)
		if [ -n "${sid}" ]; then
			sudo pkill -SIGKILL -s "${sid}" 2>/dev/null || true
		fi
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
	echo "HOST_OP_TIMEOUT: $HOST_OP_TIMEOUT seconds"
	echo "TEST_KILL_GRACE: $TEST_KILL_GRACE seconds"
	echo "TEST_SIP_SEED: $TEST_SIP_SEED"
	echo "TEST_P_SIP: $TEST_P_SIP"
	echo "TEST_R_SIP: $TEST_R_SIP"
	echo "FAIL_FAST: ${FAIL_FAST:-<not set>}"
	echo "TEST_PORT_1..4: ${TEST_PORT_1} ${TEST_PORT_2} ${TEST_PORT_3} ${TEST_PORT_4}"
	echo "DMA channels: ${TEST_DMA_ARG:-<none, the DMA cases skip themselves>}"
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
print_configuration

for test_name in "${!test_cases[@]}"; do
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
