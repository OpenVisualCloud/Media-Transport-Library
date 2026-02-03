#!/bin/bash
# shellcheck disable=SC2317

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
mtl_folder="${script_folder}/../../"
declare -A test_cases
declare -A test_results_passed
declare -A test_results_failed
declare -A test_results_skipped
declare -A test_results_total

: "${KAHAWAI_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiTest"}"
: "${KAHAWAI_UFD_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiUfdTest"}"
: "${KAHAWAI_UPL_TEST_BINARY:="${mtl_folder}/build/tests/KahawaiUplTest"}"
: "${MAX_RETRIES:=4}"
: "${RETRY_DELAY:=10}"
: "${LOG_FILE:=$(mktemp /tmp/gtest_log.XXXXXX)}"
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
	: # FAIL_FAST is already set from environment
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

retry_counter=0

# Added ${FAIL_FAST} as a workaround for long execution time caused by reruns on fleaky tests. TODO: remove.
# Added GTEST_TOTAL_SHARDS and GTEST_SHARD_INDEX to split st2110_20 tests into 2 shards as a workaround for long execution time caused by reruns on fleaky tests. TODO: remove.
generate_test_cases() {
	test_cases=()

	# Baseline suite (always run). NIGHTLY=0 must be a strict subset of NIGHTLY=1.
	test_cases["st2110_20_rx_shard0"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_20_rx_shard0.xml --gtest_filter=St20_rx*"
	test_cases["st2110_20_rx_shard1"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_20_rx_shard1.xml --gtest_filter=St20_rx*"
	test_cases["st2110_20_tx_shard0"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_20_tx_shard0.xml --gtest_filter=St20_tx*"
	test_cases["st2110_20_tx_shard1"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_20_tx_shard1.xml --gtest_filter=St20_tx*"
	test_cases["st2110_20p"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_20p.xml --gtest_filter=St20p*"
	test_cases["st2110_22_rx"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_22_rx.xml --gtest_filter=St22_rx*"
	test_cases["st2110_22_tx"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_22_tx.xml --gtest_filter=St22_tx*"
	test_cases["st2110_22p"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_22p.xml --gtest_filter=St22p*"
	test_cases["st2110_3x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_3x.xml --gtest_filter=St3*"
	test_cases["st2110_4x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st2110_4x.xml --gtest_filter=St4*"

	if [ "${NIGHTLY}" -ne 1 ]; then
		return
	fi

	# Nightly additions
	test_cases["digest_1080p_timeout_interval"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_digest_1080p_timeout_interval.xml --gtest_filter=*digest_1080p_timeout_interval*"
	test_cases["ufd_basic"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --gtest_output=xml:/tmp/gtest_ufd_basic.xml"
	test_cases["ufd_shared"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared --gtest_output=xml:/tmp/gtest_ufd_shared.xml"
	test_cases["ufd_shared_lcore"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared --udp_lcore --gtest_output=xml:/tmp/gtest_ufd_shared_lcore.xml"
	test_cases["ufd_rss"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --rss_mode l3_l4 --gtest_output=xml:/tmp/gtest_ufd_rss.xml"
	test_cases["udp_ld_preload"]="LD_PRELOAD=\"${MTL_LD_PRELOAD}\" ${KAHAWAI_UPL_TEST_BINARY} --p_sip 192.168.2.80 --r_sip 192.168.2.81 --gtest_output=xml:/tmp/gtest_udp_ld_preload.xml"
	test_cases["Misc"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_Misc.xml --gtest_filter=Misc*"
	test_cases["Main"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_Main.xml --gtest_filter=Main*"
	test_cases["Sch"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_Sch.xml --gtest_filter=Sch*"
	test_cases["Dma_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode va ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_Dma_va.xml --gtest_filter=Dma*"
	test_cases["Dma_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode pa ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_Dma_pa.xml --gtest_filter=Dma*"
	test_cases["Cvt"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_Cvt.xml --gtest_filter=Cvt*"
	test_cases["st20p_auto_pacing_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st20p_auto_pacing_pa.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_auto_pacing_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st20p_auto_pacing_va.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_tsc_pacing"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st20p_tsc_pacing.xml --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_kernel_loopback"]="\"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port kernel:lo --r_port kernel:lo ${FAIL_FAST} --gtest_output=xml:/tmp/gtest_st20p_kernel_loopback.xml --gtest_filter=St20p*"
	test_cases["noctx"]="OUTPUT_XML=/tmp/gtest_noctx.xml \"${mtl_folder}/tests/integration_tests/noctx/run.sh\"" # noctx uses script to run as it needs more setup
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
	echo "ICE driver reset completed"
	retry_counter=$((retry_counter + 1))
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

watchdog_for_configuration_errors() {
	while true; do
		sleep 15
		if ! check_configuration_errors; then
			echo "✗ Configuration error detected by watchdog. Exiting..."
			kill_test_processes
			time_taken_by_script
			exit 1
		fi
	done
}

watchdog_for_configuration_errors &

parse_gtest_results() {
	local test_name="$1"
	local xml_file="/tmp/gtest_${test_name}.xml"

	# Check if XML file exists
	if [ ! -f "$xml_file" ]; then
		echo "Warning: XML file not found: $xml_file"
		test_results_passed["$test_name"]="0"
		test_results_failed["$test_name"]="0"
		test_results_skipped["$test_name"]="0"
		test_results_total["$test_name"]="0"
		return
	fi

	# Parse XML file for test results using xmllint or grep
	local passed=0
	local failed=0
	local skipped=0
	local total=0

	# Try to use xmllint if available, otherwise fall back to grep
	if command -v xmllint >/dev/null 2>&1; then
		# Use stdin to avoid permission issues with xmllint
		# Count total testcases
		total=$(xmllint --xpath 'count(//testcase)' "$xml_file" 2>/dev/null)
		# XPath count() returns a number, handle empty or invalid output
		if [ -z "$total" ] || ! [[ "$total" =~ ^[0-9]+(.[0-9]+)?$ ]]; then
			# Fallback to grep if xmllint xpath didn't work
			total=$(grep -c '<testcase' "$xml_file" 2>/dev/null || echo "0")
			failed=$(grep -c '<failure\|<error' "$xml_file" 2>/dev/null || echo "0")
			skipped=$(grep -c 'status="notrun"' "$xml_file" 2>/dev/null || echo "0")
			# Convert float to int if needed
			total=$(printf "%.0f" "$total" 2>&1 | tail -n 1)
			total=${total:-0}
		else
			# Convert float to int if xmllint returned a decimal
			total=$(printf "%.0f" "$total" 2>&1 | tail -n 1)
			total=${total:-0}
			# Count failed tests (with failure or error tags)
			failed=$(xmllint --xpath 'count(//testcase[failure or error])' "$xml_file" 2>/dev/null)
			failed=$(printf "%.0f" "${failed:-0}" 2>&1 | tail -n 1)
			failed=${failed:-0}
			# Count skipped tests
			skipped=$(xmllint --xpath 'count(//testcase[@status="notrun"])' "$xml_file" 2>/dev/null)
			skipped=$(printf "%.0f" "${skipped:-0}" 2>&1 | tail -n 1)
			skipped=${skipped:-0}
		fi
		# Calculate passed tests
		passed=$((total - failed - skipped))
	else
		# Fallback to grep-based parsing
		total=$(grep -c '<testcase' "$xml_file" 2>/dev/null || echo "0")
		failed=$(grep -c '<failure\|<error' "$xml_file" 2>/dev/null || echo "0")
		skipped=$(grep -c 'status="notrun"' "$xml_file" 2>/dev/null || echo "0")
		passed=$((total - failed - skipped))
	fi

	# Ensure all values are integers and non-negative
	passed=$((passed > 0 ? passed : 0))
	failed=$((failed > 0 ? failed : 0))
	skipped=$((skipped > 0 ? skipped : 0))
	total=$((total > 0 ? total : 0))

	# Store results
	test_results_passed["$test_name"]="$passed"
	test_results_failed["$test_name"]="$failed"
	test_results_skipped["$test_name"]="$skipped"
	test_results_total["$test_name"]="$total"

	echo "Parsed $test_name: $passed passed, $failed failed, $skipped skipped, $total total"
}

run_test_with_retry() {
	local test_name="$1"
	local attempt=1
	local test_log_file="${LOG_FILE}.${test_name}"

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
			parse_gtest_results "$test_name"
			rm -f "$test_log_file"
			return 0
		elif (! check_configuration_errors); then
			echo "✗ Test failed due to configuration errors: $test_name (attempt $attempt/$MAX_RETRIES)" | tee -a "$LOG_FILE"
			parse_gtest_results "$test_name"
			rm -f "$test_log_file"
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
	parse_gtest_results "$test_name"
	rm -f "$test_log_file"

	if [ "$EXIT_ON_FAILURE" -eq 1 ]; then
		echo "Exiting due to test failure."
		kill_test_processes
		time_taken_by_script
		exit 1
	fi
	return 1
}

echo "Starting MTL test suite..."
echo "Maximum retries per test: $MAX_RETRIES"
echo "Retry delay: $RETRY_DELAY seconds"
echo "Exit on failure: $EXIT_ON_FAILURE"
echo "MTL_LD_PRELOAD path: $MTL_LD_PRELOAD"
echo "MUFD_CFG path: $MUFD_CFG"
echo ""

kill_test_processes

failed_tests=()
passed_tests=()

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

generate_test_cases

echo "=========================================="
echo "Test Configuration:"
echo "NIGHTLY: ${NIGHTLY}"
echo "EXIT_ON_FAILURE: ${EXIT_ON_FAILURE}"
echo "Total tests to run: ${#test_cases[@]}"
echo "=========================================="

for test_name in "${!test_cases[@]}"; do
	echo "$test_name" "${test_cases[$test_name]}"
	if run_test_with_retry "$test_name"; then
		passed_tests+=("$test_name")
	elif [ $? -eq 2 ]; then
		echo "✗ Test aborted due to configuration errors: $test_name"
		kill_test_processes
		time_taken_by_script
		exit 1
	else
		failed_tests+=("$test_name")
		# If EXIT_ON_FAILURE is enabled, stop running further tests but still print summary
		if [ "$EXIT_ON_FAILURE" -eq 1 ]; then
			echo "EXIT_ON_FAILURE is enabled, stopping test execution after first failure."
			break
		fi
	fi
done

kill_test_processes

if [ ${#passed_tests[@]} -ne 0 ]; then
	echo ""
	echo "=========================================="
	echo "Tests passed:"
	for test in "${passed_tests[@]}"; do
		echo " ✓ $test"
	done
	echo "=========================================="
fi

if [ ${#failed_tests[@]} -ne 0 ]; then
	echo ""
	echo "=========================================="
	echo "Tests failed:"
	for test in "${failed_tests[@]}"; do
		echo " - $test"
	done
	echo "=========================================="
fi

# Print detailed summary table
echo ""
echo "=========================================="
echo "TEST RESULTS SUMMARY"
echo "=========================================="
printf "%-30s | %8s | %8s | %8s | %8s | %10s\n" "Test Category" "Passed" "Failed" "Skipped" "Total" "Pass Rate"
echo "---------------------------------------------------------------------------------------------------"

total_passed=0
total_failed=0
total_skipped=0
total_tests=0

# Sort test names for consistent output - only show tests that actually ran
mapfile -t sorted_tests < <(printf '%s\n' "${!test_results_total[@]}" | sort)

for test_name in "${sorted_tests[@]}"; do
	passed="${test_results_passed[$test_name]:-0}"
	failed="${test_results_failed[$test_name]:-0}"
	skipped="${test_results_skipped[$test_name]:-0}"
	total="${test_results_total[$test_name]:-0}"

	# If total is 0, try to calculate from components
	if [ "$total" -eq 0 ] && { [ "$passed" -gt 0 ] || [ "$failed" -gt 0 ] || [ "$skipped" -gt 0 ]; }; then
		total=$((passed + failed + skipped))
	fi

	# Skip tests with no results (didn't run or no XML generated)
	if [ "$total" -eq 0 ]; then
		continue
	fi

	# Calculate pass rate
	if [ "$total" -gt 0 ]; then
		pass_rate=$(awk "BEGIN {printf \"%.2f\", ($passed / $total) * 100}")
	else
		pass_rate="N/A"
	fi

	printf "%-30s | %8d | %8d | %8d | %8d | %9s%%\n" \
		"$test_name" "$passed" "$failed" "$skipped" "$total" "$pass_rate"

	total_passed=$((total_passed + passed))
	total_failed=$((total_failed + failed))
	total_skipped=$((total_skipped + skipped))
	total_tests=$((total_tests + total))
done

echo "---------------------------------------------------------------------------------------------------"

# Calculate overall pass rate
if [ "$total_tests" -gt 0 ]; then
	overall_pass_rate=$(awk "BEGIN {printf \"%.2f\", ($total_passed / $total_tests) * 100}")
else
	overall_pass_rate="0.00"
fi

printf "%-30s | %8d | %8d | %8d | %8d | %9s%%\n" \
	"TOTAL" "$total_passed" "$total_failed" "$total_skipped" "$total_tests" "$overall_pass_rate"

echo "=========================================="
echo ""
echo "Summary:"
echo "  Total test categories: ${#test_cases[@]}"
echo "  Categories attempted: $((${#passed_tests[@]} + ${#failed_tests[@]}))"
echo "  Categories passed: ${#passed_tests[@]}"
echo "  Categories failed: ${#failed_tests[@]}"
echo "  Overall pass rate: ${overall_pass_rate}%"
echo "=========================================="

if [ ${#failed_tests[@]} -ne 0 ]; then
	time_taken_by_script
	exit 1
fi

time_taken_by_script
exit 0
