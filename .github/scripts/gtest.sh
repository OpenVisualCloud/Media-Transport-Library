#!/bin/bash
# shellcheck disable=SC2317

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

# sudo strips LD_LIBRARY_PATH even with -E; pass it explicitly via env
SUDO_PREFIX="sudo -E env LD_LIBRARY_PATH=${LD_LIBRARY_PATH} PATH=${PATH}"

: "${MAX_RETRIES:=2}"
: "${RETRY_DELAY:=20}"
: "${TMP_FOLDER:=/tmp/mtl_gtest_$(date +%Y%m%d_%H%M%S)_$$}"
: "${LOG_FILE:=${TMP_FOLDER}/gtest.log}"
: "${EXIT_ON_FAILURE:=1}"
: "${NIGHTLY:=1}"                                                                    # Set to 1 to run full test suite, 0 for quick tests
: "${TEST_CASE_TIMEOUT:=1800}"                                                       # 30 minutes per test case
: "${HOST_OP_TIMEOUT:=180}"                                                          # Hard bound for one modprobe/nicctl/devbind call
: "${TEST_KILL_GRACE:=30}"                                                           # SIGKILL delay after SIGTERM for a test case
: "${MIN_VFIO_PORTS:=4}"                                                             # Ports the suite needs bound to vfio-pci
: "${HOST_FAULT_EXIT:=3}"                                                            # Exit code meaning "host needs recovery, not a test failure"
: "${TEST_SIP_SEED:=$((RANDOM))}"                                                    # Seed for generating TEST_P_SIP when not provided
: "${TEST_P_SIP:="192.168.$((TEST_SIP_SEED % 256)).$((TEST_SIP_SEED % 256))"}"       # Primary test IP for gtest
: "${TEST_R_SIP:="192.168.$((TEST_SIP_SEED % 256)).$(((TEST_SIP_SEED + 1) % 256))"}" # Remote test IP for gtest

if [ "${NIGHTLY}" -eq 0 ]; then
	FAIL_FAST="--gtest_fail_fast" # Skips remaining tests on first failure
else
	FAIL_FAST=""
fi

for dma in "CBDMA" "idxd" "ioatdma"; do
	if dpdk-devbind.py --status-dev dma | grep -q "$dma"; then
		export dma_mechanism="$dma"
		break
	fi
done

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

# Use fail-fast for quick tests, and sharding for st2110_20 to reduce execution time
generate_test_cases() {
	test_cases=()

	# Baseline suite (always run). NIGHTLY=0 must be a strict subset of NIGHTLY=1.
	test_cases["st2110_20_rx_shard0"]="${SUDO_PREFIX} GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_rx_shard0.xml --gtest_filter=St20_rx*"
	test_cases["st2110_20_rx_shard1"]="${SUDO_PREFIX} GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_rx_shard1.xml --gtest_filter=St20_rx*"
	test_cases["st2110_20_tx_shard0"]="${SUDO_PREFIX} GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_tx_shard0.xml --gtest_filter=St20_tx*"
	test_cases["st2110_20_tx_shard1"]="${SUDO_PREFIX} GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_tx_shard1.xml --gtest_filter=St20_tx*"
	test_cases["st2110_20p"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20p.xml --gtest_filter=St20p*"
	test_cases["st2110_22_rx"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_22_rx.xml --gtest_filter=St22_rx*"
	test_cases["st2110_22_tx"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_22_tx.xml --gtest_filter=St22_tx*"
	test_cases["st2110_22p"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_22p.xml --gtest_filter=St22p*"
	test_cases["st2110_3x"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_3x.xml --gtest_filter=St3*"
	test_cases["st2110_4x"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_4x.xml --gtest_filter=St4*"

	if [ "${NIGHTLY}" -ne 1 ]; then
		return
	fi

	# Nightly additions
	test_cases["digest_1080p_timeout_interval"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_digest_1080p_timeout_interval.xml --gtest_filter=*digest_1080p_timeout_interval*"
	test_cases["Misc"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Misc.xml --gtest_filter=Misc*"
	test_cases["Main"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Main.xml --gtest_filter=Main*"
	test_cases["Sch"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Sch.xml --gtest_filter=Sch*"
	test_cases["Dma_va"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode va ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Dma_va.xml --gtest_filter=Dma*"
	test_cases["Dma_pa"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode pa ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Dma_pa.xml --gtest_filter=Dma*"
	test_cases["Cvt"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Cvt.xml --gtest_filter=Cvt*"
	test_cases["st20p_auto_pacing_pa"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_auto_pacing_pa.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_auto_pacing_va"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_auto_pacing_va.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_tsc_pacing"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_tsc_pacing.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_kernel_loopback"]="\"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\"  --auto_start_stop --p_port kernel:lo --r_port kernel:lo ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_kernel_loopback.xml --gtest_filter=St20p*"
	test_cases["st20s"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20s.xml --gtest_filter=St20s*"
	test_cases["redundant_stats"]="${SUDO_PREFIX} \"${KAHAWAI_TEST_BINARY}\" --p_sip=\"${TEST_P_SIP}\" --auto_start_stop --port_list \"${TEST_PORT_1},${TEST_PORT_2},${TEST_PORT_3},${TEST_PORT_4}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_redundant_stats.xml --gtest_filter=St20p.redundant*:St30p.redundant*:St40p.redundant*"
	test_cases["noctx"]="\"${mtl_folder}/tests/integration_tests/noctx/run.sh\""
}

dump_driver_state() {
	echo "--- processes ---"
	ps -eo pid,stat,wchan:32,etimes,args | grep -E 'modprobe|KahawaiTest|nicctl|dpdk-devbind' | grep -v grep || true
	local modprobe_pid
	modprobe_pid=$(pgrep -x modprobe | head -1)
	if [ -n "${modprobe_pid}" ]; then
		echo "--- /proc/${modprobe_pid}/stack ---"
		sudo cat "/proc/${modprobe_pid}/stack" 2>/dev/null || true
	fi
	echo "--- modules ---"
	lsmod | grep -E '^ice|^vfio' || true
	echo "--- dmesg tail ---"
	sudo dmesg -T 2>/dev/null | tail -50 || true
}

# A wedged ICE probe leaves modprobe in uninterruptible sleep, where not even
# SIGKILL reclaims it. The script cannot fix that, but it must stop waiting:
# an unbounded wait holds a fleet runner for GitHub's 360-minute default.
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

# Runs one host-configuration command under a hard timeout and treats expiry
# as a host fault. Nothing here is retried: re-running a command that already
# hung on a faulted driver only wedges the host further.
run_bounded() {
	local label="$1"
	shift
	local retval=0
	timeout --foreground --signal=SIGTERM --kill-after="${TEST_KILL_GRACE}" \
		"${HOST_OP_TIMEOUT}" "$@" || retval=$?
	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		host_fault "${label} did not finish within ${HOST_OP_TIMEOUT}s"
	fi
	return "${retval}"
}

# nicctl.sh touches the same driver, so it can block the same way. It runs in
# command substitution, where a return code is lost, so a timeout is recorded
# in a sentinel file that the callers check before reporting "no ports found".
nicctl_list() {
	local retval=0
	timeout --foreground --signal=SIGTERM --kill-after="${TEST_KILL_GRACE}" \
		"${HOST_OP_TIMEOUT}" "${mtl_folder}/script/nicctl.sh" list "$@" 2>/dev/null || retval=$?
	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		touch "${TMP_FOLDER}/.nicctl_timeout"
	fi
	return "${retval}"
}

nicctl_wedged() {
	[ -f "${TMP_FOLDER}/.nicctl_timeout" ]
}

vfio_port_count() {
	nicctl_list all | awk '$3 == "vfio-pci"' | wc -l
}

# The host is already in the state the suite needs when the ICE module is
# loaded and enough ports are bound to vfio-pci. Reloading in that state buys
# nothing and is another chance to hit the ICE probe fault.
ice_state_is_usable() {
	lsmod | awk '{print $1}' | grep -qwx "ice" || return 1
	[ "$(vfio_port_count)" -ge "${MIN_VFIO_PORTS}" ]
}

# This should never be run with active proccesses using dpdk running, as it could lead to hangs
bind_driver_to_dpdk() {
	echo binding driver to DPDK...

	if ! lsmod | awk '{print $1}' | grep -wx "ice"; then
		echo "ICE driver not loaded, loading..."
		if run_bounded "modprobe ice" sudo modprobe ice; then
			sleep 3
		else
			echo "Warning: Failed to load ICE driver"
			time_taken_by_script
			exit 1
		fi
	fi

	if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ] || [ -z "$TEST_PORT_3" ] || [ -z "$TEST_PORT_4" ]; then
		nicctl_list up

		found_match=false
		for numa in 0 1 2 3; do
			pfs=$(nicctl_list up | awk -v numa="${numa}" '$3 == "ice" && $4 == numa {print $2}')
			echo "Found ICE PFs on NUMA node ${numa}: $pfs"

			for p in $pfs; do
				TEST_DMA_PORT_P=$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v numa="${numa}" '$0 ~ mech && $0 ~ ("numa_node=" numa) {print $1; exit}')
				TEST_DMA_PORT_R=$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v numa="${numa}" -v p="${TEST_DMA_PORT_P}" '$0 ~ mech && $0 ~ ("numa_node=" numa) && $1 != p {print $1; exit}')
				if [ -n "${TEST_DMA_PORT_P}" ] && [ -n "${TEST_DMA_PORT_R}" ]; then
					export pf_numa="${numa}"
					export pf="${p}"
					found_match=true
					break
				elif [ -n "${TEST_DMA_PORT_P}" ] && [ -n "$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v p="${TEST_DMA_PORT_P}" '$0 ~ mech && $1 != p {print $1}' | head -1)" ]; then
					export TEST_DMA_PORT_LEPSZYRYDZNIZNICA="${TEST_DMA_PORT_P}"
					TEST_DMA_PORT_LEPSZYRYDZNIZNICB="$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v p="${TEST_DMA_PORT_P}" '$0 ~ mech && $1 != p {print $1}' | head -1)"
					export TEST_DMA_PORT_LEPSZYRYDZNIZNICB
					pf_lepszyrydzniznica="${p}"
					export pf_lepszyrydzniznica
				fi
			done
			[ "$found_match" = true ] && break
		done

		if [ -z "${pf}" ] || [ -z "$TEST_DMA_PORT_P" ] || [ -z "$TEST_DMA_PORT_R" ]; then
			echo "Error: Could not find ICE PF with matching DMA ports"
			if [ -n "${TEST_DMA_PORT_LEPSZYRYDZNIZNICA}" ] && [ -n "${TEST_DMA_PORT_LEPSZYRYDZNIZNICB}" ]; then
				echo "Found DMA ports without matching numa node: $TEST_DMA_PORT_LEPSZYRYDZNIZNICA, $TEST_DMA_PORT_LEPSZYRYDZNIZNICB"
				export TEST_DMA_PORT_P="${TEST_DMA_PORT_LEPSZYRYDZNIZNICA}"
				export TEST_DMA_PORT_R="${TEST_DMA_PORT_LEPSZYRYDZNIZNICB}"
				export pf="${pf_lepszyrydzniznica}"
			else
				echo "No suitable DMA ports found either"
				if nicctl_wedged; then
					host_fault "nicctl.sh stopped responding while listing NIC ports"
				fi
				time_taken_by_script
				exit 1
			fi
		fi

		pf_numa=$(dpdk-devbind.py --status-dev net | grep "$pf" | awk -F 'numa_node=' '{print $2}' | awk '{print $1}')
		echo "Binding PF $pf to DPDK driver numa node ${pf_numa}"
		run_bounded "nicctl create_tvf ${pf}" sudo -E "${mtl_folder}/script/nicctl.sh" create_tvf "$pf"
	fi

	sleep 5
	TEST_PORT_1=$(nicctl_list all | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_2=$(nicctl_list all | grep -v "${TEST_PORT_1}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_3=$(nicctl_list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_4=$(nicctl_list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | grep -v "${TEST_PORT_3}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)

	if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ] || [ -z "$TEST_PORT_3" ] || [ -z "$TEST_PORT_4" ]; then
		if nicctl_wedged; then
			host_fault "nicctl.sh stopped responding while listing NIC ports"
		fi
		echo "Error: Could not find enough VFIO-PCI bound ports for testing"
		echo " TEST_PORT_1=$TEST_PORT_1"
		echo " TEST_PORT_2=$TEST_PORT_2"
		echo " TEST_PORT_3=$TEST_PORT_3"
		echo " TEST_PORT_4=$TEST_PORT_4"
		time_taken_by_script
		exit 1
	fi

	# for the noctx tests
	export TEST_PORT_1
	export TEST_PORT_2
	export TEST_PORT_3
	export TEST_PORT_4

	echo "Selected ports: P=$TEST_PORT_1, R=$TEST_PORT_2"

	if ! dpdk-devbind.py --status-dev dma | grep "$TEST_DMA_PORT_P" | grep -q "unused=${dma_mechanism}"; then
		if ! run_bounded "dpdk-devbind DMA port P" sudo dpdk-devbind.py --bind vfio-pci "$TEST_DMA_PORT_P"; then
			echo "Error: Could not bind DMA port P ($TEST_DMA_PORT_P) to vfio-pci"
			time_taken_by_script
			exit 1
		fi
		echo "Successfully bound DMA port P $(dpdk-devbind.py --status-dev dma | grep "$TEST_DMA_PORT_P") to vfio-pci"
	else
		echo "DMA port P $(dpdk-devbind.py --status-dev dma | grep "$TEST_DMA_PORT_P") already bound to vfio-pci"
	fi

	if ! dpdk-devbind.py --status-dev dma | grep "$TEST_DMA_PORT_R" | grep -q "unused=${dma_mechanism}"; then
		if ! run_bounded "dpdk-devbind DMA port R" sudo dpdk-devbind.py --bind vfio-pci "$TEST_DMA_PORT_R"; then
			echo "Error: Could not bind DMA port R ($TEST_DMA_PORT_R) to vfio-pci"
			time_taken_by_script
			exit 1
		fi
		echo "Successfully bound DMA port R $(dpdk-devbind.py --status-dev dma | grep "$TEST_DMA_PORT_R") to vfio-pci"
	else
		echo "DMA port R $(dpdk-devbind.py --status-dev dma | grep "$TEST_DMA_PORT_R") already bound to vfio-pci"
	fi

	generate_test_cases
}

reset_ice_driver() {
	if ice_state_is_usable; then
		echo "ICE loaded with at least ${MIN_VFIO_PORTS} vfio-pci ports, skipping reload"
	else
		echo "Resetting ICE driver..."
		run_bounded "modprobe -r ice" sudo modprobe -r ice || true
		sleep 5
		run_bounded "modprobe ice" sudo modprobe ice || true
		sleep 10
	fi

	export TEST_PORT_1=""
	export TEST_PORT_2=""
	export TEST_PORT_3=""
	export TEST_PORT_4=""
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

	setsid --wait bash -c 'echo $$ >"$1"; exec timeout --signal=SIGTERM --kill-after="$2" "$3" bash -c "$4"' \
		gtest-case "${sid_file}" "${TEST_KILL_GRACE}" "${TEST_CASE_TIMEOUT}" "${test_cases[$test_name]}" \
		>>"${case_log}" 2>&1 || retval=$?

	kill "${tail_pid}" 2>/dev/null || true
	wait "${tail_pid}" 2>/dev/null || true
	sudo tee -a "$LOG_FILE" <"${case_log}" >/dev/null

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

				reset_ice_driver
				bind_driver_to_dpdk
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
	echo "Starting MTL test suite..."
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
	echo "MIN_VFIO_PORTS: $MIN_VFIO_PORTS"
	echo "TEST_SIP_SEED: $TEST_SIP_SEED"
	echo "TEST_P_SIP: $TEST_P_SIP"
	echo "TEST_R_SIP: $TEST_R_SIP"
	echo "FAIL_FAST: ${FAIL_FAST:-<not set>}"
	echo "dma_mechanism: ${dma_mechanism:-<not set>}"
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

print_configuration

kill_test_processes

reset_ice_driver
bind_driver_to_dpdk

if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ]; then
	echo "Error: TEST_PORT_1 or TEST_PORT_2 environment variables are not set"
	echo "TEST_PORT_1=$TEST_PORT_1"
	echo "TEST_PORT_2=$TEST_PORT_2"
	time_taken_by_script
	exit 1
fi

if [ -z "$TEST_DMA_PORT_P" ] || [ -z "$TEST_DMA_PORT_R" ]; then
	echo "Error: TEST_DMA_PORT_P or TEST_DMA_PORT_R environment variables are not set"
	echo "TEST_DMA_PORT_P=$TEST_DMA_PORT_P"
	echo "TEST_DMA_PORT_R=$TEST_DMA_PORT_R"
	time_taken_by_script
	exit 1
fi

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
