#!/bin/bash
# shellcheck disable=SC2317

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
mtl_folder="${script_folder}/../../"
declare -A test_cases

: "${KAHAWAI_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiTest"}"
: "${KAHAWAI_UFD_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiUfdTest"}"
: "${KAHAWAI_UPL_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiUplTest"}"
: "${MAX_RETRIES:=4}"
: "${RETRY_DELAY:=10}"
# Create unique temporary directory for this test run to avoid permission issues and preserve history
: "${TMP_FOLDER:=/tmp/mtl_gtest_$(date +%Y%m%d_%H%M%S)_$$}"
mkdir -p "$TMP_FOLDER"
export TMP_FOLDER
: "${LOG_FILE:=${TMP_FOLDER}/gtest.log}"
: "${EXIT_ON_FAILURE:=1}"
: "${MTL_LD_PRELOAD:=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so}"
: "${MUFD_CFG:="${mtl_folder}/.github/workflows/upl_gtest.json"}"
: "${NIGHTLY:=1}" # Set to 1 to run full test suite, 0 for quick tests
: "${PF_NUMA:=}"
: "${PF_INDEX:=0}"
: "${TEST_CASE_TIMEOUT:=1800}" # 30 minutes per test case

# Signal trap for cleanup on termination
cleanup() {
	echo "Caught signal, cleaning up..."
	kill_test_processes
	kill -- -$$ 2>/dev/null || true
	exit 130
}
trap cleanup SIGINT SIGTERM SIGHUP

# Enable fail-fast only for quick tests (NIGHTLY=0)
if [ "${NIGHTLY}" -eq 0 ]; then
	FAIL_FAST="--gtest_fail_fast"
else
	FAIL_FAST=""
fi

if [[ ! "${PF_INDEX}" =~ ^[0-9]+$ ]]; then
	PF_INDEX=0
fi

echo "Log file: $LOG_FILE"

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
	test_cases["st2110_20_rx_shard0"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_rx_shard0.xml --gtest_filter=St20_rx*"
	test_cases["st2110_20_rx_shard1"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_rx_shard1.xml --gtest_filter=St20_rx*"
	test_cases["st2110_20_tx_shard0"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_tx_shard0.xml --gtest_filter=St20_tx*"
	test_cases["st2110_20_tx_shard1"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20_tx_shard1.xml --gtest_filter=St20_tx*"
	test_cases["st2110_20p"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_20p.xml --gtest_filter=St20p*"
	test_cases["st2110_22_rx"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_22_rx.xml --gtest_filter=St22_rx*"
	test_cases["st2110_22_tx"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_22_tx.xml --gtest_filter=St22_tx*"
	test_cases["st2110_22p"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_22p.xml --gtest_filter=St22p*"
	test_cases["st2110_3x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_3x.xml --gtest_filter=St3*"
	test_cases["st2110_4x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st2110_4x.xml --gtest_filter=St4*"

	if [ "${NIGHTLY}" -ne 1 ]; then
		return
	fi

	# Nightly additions
	test_cases["digest_1080p_timeout_interval"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_digest_1080p_timeout_interval.xml --gtest_filter=*digest_1080p_timeout_interval*"
	test_cases["ufd_basic"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --gtest_output=xml:${TMP_FOLDER}/gtest_ufd_basic.xml"
	test_cases["ufd_shared"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared --gtest_output=xml:${TMP_FOLDER}/gtest_ufd_shared.xml"
	test_cases["ufd_shared_lcore"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared --udp_lcore --gtest_output=xml:${TMP_FOLDER}/gtest_ufd_shared_lcore.xml"
	test_cases["ufd_rss"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --rss_mode l3_l4 --gtest_output=xml:${TMP_FOLDER}/gtest_ufd_rss.xml"
	test_cases["udp_ld_preload"]="LD_PRELOAD=\"${MTL_LD_PRELOAD}\" ${KAHAWAI_UPL_TEST_BINARY} --p_sip 192.168.2.80 --r_sip 192.168.2.81 --gtest_output=xml:${TMP_FOLDER}/gtest_udp_ld_preload.xml"
	test_cases["Misc"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Misc.xml --gtest_filter=Misc*"
	test_cases["Main"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Main.xml --gtest_filter=Main*"
	test_cases["Sch"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Sch.xml --gtest_filter=Sch*"
	test_cases["Dma_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode va ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Dma_va.xml --gtest_filter=Dma*"
	test_cases["Dma_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode pa ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Dma_pa.xml --gtest_filter=Dma*"
	test_cases["Cvt"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_Cvt.xml --gtest_filter=Cvt*"
	test_cases["st20p_auto_pacing_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_auto_pacing_pa.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_auto_pacing_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_auto_pacing_va.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_tsc_pacing"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_tsc_pacing.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_kernel_loopback"]="\"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port kernel:lo --r_port kernel:lo ${FAIL_FAST} --gtest_output=xml:${TMP_FOLDER}/gtest_st20p_kernel_loopback.xml --gtest_filter=St20p*"
	test_cases["noctx"]="\"${mtl_folder}/tests/integration_tests/noctx/run.sh\""
}

bind_driver_to_dpdk() {
	if ! lsmod | awk '{print $1}' | grep -wx "ice"; then
		echo "ICE driver not loaded, loading..."
		if sudo modprobe ice; then
			sleep 3
		else
			echo "Warning: Failed to load ICE driver"
			time_taken_by_script
			exit 1
		fi
	fi
	TEST_PORT_1=$("${mtl_folder}/script/nicctl.sh" list all | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_2=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_3=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_4=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | grep -v "${TEST_PORT_3}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)

	if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ] || [ -z "$TEST_PORT_3" ] || [ -z "$TEST_PORT_4" ]; then
		if [ -z "${pf}" ]; then
			if [ -n "${PF_NUMA}" ]; then
				pf=$("${mtl_folder}/script/nicctl.sh" list all | awk -v numa="${PF_NUMA}" -v idx="${PF_INDEX}" '$3 == "ice" && $4 == numa { if (c == idx) {print $2; exit} c++ }')
			else
				pf=$("${mtl_folder}/script/nicctl.sh" list all | awk -v idx="${PF_INDEX}" '$3 == "ice" { if (c == idx) {print $2; exit} c++ }')
			fi

			if [ -z "${pf}" ]; then
				echo "Error: Could not find ICE PF (PF_NUMA='${PF_NUMA:-}', PF_INDEX='${PF_INDEX}')"
				time_taken_by_script
				exit 1
			fi
		fi

		echo "Binding PF $pf to DPDK driver"
		sudo -E "${mtl_folder}/script/nicctl.sh" create_tvf "$pf"

		TEST_PORT_1=$("${mtl_folder}/script/nicctl.sh" list all | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
		TEST_PORT_2=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
		TEST_PORT_3=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
		TEST_PORT_4=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | grep -v "${TEST_PORT_3}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	fi

	# for the noctx tests
	export TEST_PORT_1
	export TEST_PORT_2
	export TEST_PORT_3
	export TEST_PORT_4

	if [ ! -f "${mtl_folder}/.github/workflows/upl_gtest_template.json" ]; then
		echo "Error: Template file not found: ${mtl_folder}/.github/workflows/upl_gtest_template.json"
		time_taken_by_script
		exit 1
	fi

	mkdir -p "$(dirname "$MUFD_CFG")" || true
	cp -f "${mtl_folder}/.github/workflows/upl_gtest_template.json" "$MUFD_CFG"
	export MUFD_CFG

	sed -i "s+REPLACE_BY_CICD_TEST_PORT_1+${TEST_PORT_1}+" "$MUFD_CFG"
	sed -i "s+REPLACE_BY_CICD_TEST_PORT_2+${TEST_PORT_2}+" "$MUFD_CFG"
	echo "Selected ports: P=$TEST_PORT_1, R=$TEST_PORT_2"

	for dma_mechanism in "CBDMA" "idxd" "ioatdma"; do
		if [ -n "${PF_NUMA}" ]; then
			TEST_DMA_PORT_P=$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v numa="${PF_NUMA}" '$0 ~ mech && $0 ~ ("numa_node=" numa) {print $1; exit}')
			TEST_DMA_PORT_R=$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v numa="${PF_NUMA}" -v p="${TEST_DMA_PORT_P}" '$0 ~ mech && $0 ~ ("numa_node=" numa) && $1 != p {print $1; exit}')
		else
			TEST_DMA_PORT_P=$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" '$0 ~ mech {print $1; exit}')
			TEST_DMA_PORT_R=$(dpdk-devbind.py -s | awk -v mech="$dma_mechanism" -v p="${TEST_DMA_PORT_P}" '$0 ~ mech && $1 != p {print $1; exit}')
		fi
		if [ -n "$TEST_DMA_PORT_P" ] && [ -n "$TEST_DMA_PORT_R" ]; then
			break
		fi
	done

	if [ -z "$TEST_DMA_PORT_P" ] || [ -z "$TEST_DMA_PORT_R" ]; then
		echo "Error: Could not find suitable DPDK DMA devices"
		time_taken_by_script
		exit 1
	fi

	generate_test_cases
}

reset_ice_driver() {
	echo "Resetting ICE driver..."
	sudo modprobe -r ice || true
	sleep 5
	sudo modprobe ice || true
	sleep 10
}

kill_test_processes() {
	# Kill by process group if available
	pkill -SIGKILL -P $$ 2>/dev/null || true
	sudo killall -SIGKILL KahawaiTest KahawaiUfdTest KahawaiUplTest 2>/dev/null || true
	sleep 2
}

# These messages suggest configuration errors that require manual intervention
# If those are found in log just give up immediately
declare -a error_messages=(
	"Not a directory"
	"mt_user_params_check, same name for port 1 and 0"
	"get socket fail -19 for pmd 0"
	"EAL: Cannot use IOVA as"
	"from LD_PRELOAD cannot be preloaded"
	"Error: ufd_parse_json, open json file ufd.json fail"
	"libmtl.so: cannot open shared object file:"
	"EAL: Cannot set up DMA remapping, error 12 (Cannot allocate memory)"
	"Error: mt_user_params_check, same name  for port 1 and 0"
	"Error: mt_user_params_check(1), invalid ip 0.0.0.0"
	"cannot open shared object file: No such file or directory"
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

run_test_with_retry() {
	local test_name="$1"
	local attempt=1

	echo "=========================================="
	echo "Running: $test_name" | tee -a "$LOG_FILE"
	echo "Command: ${test_cases[$test_name]}" | tee -a "$LOG_FILE"
	echo "=========================================="

	while [ $attempt -le "$MAX_RETRIES" ]; do
		echo "Attempt $attempt/$MAX_RETRIES for: $test_name"

		timeout --signal=SIGKILL "${TEST_CASE_TIMEOUT}" bash -c "${test_cases[$test_name]}" 2>&1 | tee -a "$LOG_FILE"
		RETVAL=${PIPESTATUS[0]}
		if [[ $RETVAL == 0 ]]; then
			echo "✓ Test passed: $test_name" | tee -a "$LOG_FILE"
			return 0
		elif (! check_configuration_errors); then
			echo "✗ Test failed due to configuration errors: $test_name (attempt $attempt/$MAX_RETRIES)" | tee -a "$LOG_FILE"
			return 2
		else
			echo "✗ Attempt failed for $test_name (attempt $attempt/$MAX_RETRIES)" | tee -a "$LOG_FILE"

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

	echo "✗ Test failed after $MAX_RETRIES attempts: $test_name" | tee -a "$LOG_FILE"

	if [ "$EXIT_ON_FAILURE" -eq 1 ]; then
		echo "Exiting due to test failure."
		kill_test_processes
		time_taken_by_script
		exit 1
	fi
	return 1
}

echo "Starting MTL test suite..."
echo "NIGHTLY: ${NIGHTLY}"
echo "Maximum retries per test: $MAX_RETRIES"
echo "Retry delay: $RETRY_DELAY seconds"
echo "Exit on failure: $EXIT_ON_FAILURE"
echo "MTL_LD_PRELOAD path: $MTL_LD_PRELOAD"
echo "MUFD_CFG path: $MUFD_CFG"
echo ""

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

exit 0
