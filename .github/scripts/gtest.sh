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
: "${LOG_FILE:=$(mktemp /tmp/gtest_log.XXXXXX)}"
: "${EXIT_ON_FAILURE:=1}"
: "${MTL_LD_PRELOAD:=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so}"
: "${MUFD_CFG:="${mtl_folder}/.github/workflows/upl_gtest.json"}"
: "${NIGHTLY:=1}" # Set to 1 to run full test suite, 0 for quick tests
: "${PF_NUMA:=}"
: "${PF_INDEX:=0}"

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

retry_counter=0

# Added --gtest_fail_fast as a workaround for long execution time caused by reruns on fleaky tests. TODO: remove.
# Added GTEST_TOTAL_SHARDS and GTEST_SHARD_INDEX to split st2110_20 tests into 2 shards as a workaround for long execution time caused by reruns on fleaky tests. TODO: remove.
generate_test_cases() {
	test_cases=()

	# Baseline suite (always run). NIGHTLY=0 must be a strict subset of NIGHTLY=1.
	test_cases["st2110_20_rx_shard0"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St20_rx*"
	test_cases["st2110_20_rx_shard1"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St20_rx*"
	test_cases["st2110_20_tx_shard0"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=0 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St20_tx*"
	test_cases["st2110_20_tx_shard1"]="sudo -E env GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St20_tx*"
	test_cases["st2110_20p"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St20p*"
	test_cases["st2110_22_rx"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St22_rx*"
	test_cases["st2110_22_tx"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St22_tx*"
	test_cases["st2110_22p"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St22p*"
	test_cases["st2110_3x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St3*"
	test_cases["st2110_4x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=St4*"

	if [ "${NIGHTLY}" -ne 1 ]; then
		return
	fi

	# Nightly additions
	test_cases["digest_1080p_timeout_interval"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_filter=*digest_1080p_timeout_interval*"
	test_cases["ufd_basic"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\""
	test_cases["ufd_shared"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared"
	test_cases["ufd_shared_lcore"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared --udp_lcore"
	test_cases["ufd_rss"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --rss_mode l3_l4"
	test_cases["udp_ld_preload"]="LD_PRELOAD=\"${MTL_LD_PRELOAD}\" ${KAHAWAI_UPL_TEST_BINARY} --p_sip 192.168.2.80 --r_sip 192.168.2.81"
	test_cases["Misc"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=Misc*"
	test_cases["Main"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=Main*"
	test_cases["Sch"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=Sch*"
	test_cases["Dma_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode va ${FAIL_FAST} --gtest_filter=Dma*"
	test_cases["Dma_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode pa ${FAIL_FAST} --gtest_filter=Dma*"
	test_cases["Cvt"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" ${FAIL_FAST} --gtest_filter=Cvt*"
	test_cases["st20p_auto_pacing_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port ${FAIL_FAST} --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_auto_pacing_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_tsc_pacing"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode va --multi_src_port ${FAIL_FAST} --gtest_filter=Main*:St20p*:-*ext*"
	test_cases["st20p_kernel_loopback"]="\"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port kernel:lo --r_port kernel:lo ${FAIL_FAST} --gtest_filter=St20p*"
	test_cases["noctx"]="\"${mtl_folder}/tests/integration_tests/noctx/run.sh\"" # noctx uses script to run as it needs more setup
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
	sudo killall -SIGINT KahawaiTest >/dev/null 2>&1 || true
	sudo killall -SIGINT KahawaiUfdTest >/dev/null 2>&1 || true
	sudo killall -SIGINT KahawaiUplTest >/dev/null 2>&1 || true
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

		eval "${test_cases[$test_name]}" 2>&1 | tee -a "$LOG_FILE"
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
	time_taken_by_script
	exit 1
fi

time_taken_by_script
exit 0
