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
: "${MAX_RETRIES:=3}"
: "${RETRY_DELAY:=10}"
: "${LOG_FILE:=$(mktemp /tmp/gtest_log.XXXXXX)}"
: "${EXIT_ON_FAILURE:=1}"
: "${MTL_LD_PRELOAD:=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so}"
: "${MUFD_CFG:="${mtl_folder}/.github/workflows/upl_gtest.json"}"
: "${NIGHTLY:=1}" # Set to 1 to run full test suite, 0 for quick tests
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

cleanup_on_signal() {
	echo "Received termination signal. Cleaning up..."
	kill_test_processes
	time_taken_by_script
	exit 130
}
trap cleanup_on_signal SIGINT SIGTERM

retry_counter=0

generate_test_cases() {
	if [ "${NIGHTLY}" -eq 1 ]; then
		test_cases["digest_1080p_timeout_interval"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode pa --multi_src_port --gtest_filter=*digest_1080p_timeout_interval*"
		test_cases["ufd_basic"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\""
		test_cases["ufd_shared"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared"
		test_cases["ufd_shared_lcore"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --queue_mode shared --udp_lcore"
		test_cases["ufd_rss"]="\"${KAHAWAI_UFD_TEST_BINARY}\" --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --rss_mode l3_l4"
		test_cases["udp_ld_preload"]="LD_PRELOAD=\"${MTL_LD_PRELOAD}\" ${KAHAWAI_UPL_TEST_BINARY} --p_sip 192.168.2.80 --r_sip 192.168.2.81"
		test_cases["Misc"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=Misc*"
		test_cases["Main"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=Main*"
		test_cases["Sch"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=Sch*"
		test_cases["Dma_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode va --gtest_filter=Dma*"
		test_cases["Dma_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode pa --gtest_filter=Dma*"
		test_cases["Cvt"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=Cvt*"
		test_cases["st2110_20"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St20*:St21*"
		test_cases["st2110_22"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St22*"
		test_cases["st2110_3x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St3*"
		test_cases["st2110_4x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St4*"
		test_cases["st20p_auto_pacing_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port --gtest_filter=Main*:St20p*:-*ext*"
		test_cases["st20p_auto_pacing_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode va --multi_src_port --gtest_filter=Main*:St20p*:-*ext*"
		test_cases["st20p_tsc_pacing"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way tsc --iova_mode va --multi_src_port --gtest_filter=Main*:St20p*:-*ext*"
		test_cases["st20p_kernel_loopback"]="\"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port kernel:lo --r_port kernel:lo --gtest_filter=St20p*"
		test_cases["noctx"]="\"${mtl_folder}/tests/integration_tests/noctx/run.sh\"" # noctx uses script to run as it needs more setup
	else
		test_cases["st20p_auto_pacing_va"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --rss_mode l3_l4 --pacing_way auto --iova_mode pa --multi_src_port --gtest_filter=Main*:St20p*:-*ext*"
		test_cases["Dma_pa"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --iova_mode pa --gtest_filter=Dma*"
		test_cases["st2110_20"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St20*:St21*"
		test_cases["st2110_22"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St22*"
		test_cases["st2110_3x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St3*"
		test_cases["st2110_4x"]="sudo -E \"${KAHAWAI_TEST_BINARY}\" --auto_start_stop --p_port \"${TEST_PORT_1}\" --r_port \"${TEST_PORT_2}\" --dma_dev \"${TEST_DMA_PORT_P},${TEST_DMA_PORT_R}\" --gtest_filter=St4*"
	fi
}

bind_driver_to_dpdk() {
	sudo modprobe ice || true
	TEST_PORT_1=$("${mtl_folder}/script/nicctl.sh" list all | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_2=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_3=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)
	TEST_PORT_4=$("${mtl_folder}/script/nicctl.sh" list all | grep -v "${TEST_PORT_1}" | grep -v "${TEST_PORT_2}" | grep -v "${TEST_PORT_3}" | awk '$3 == "vfio-pci" {print $2}' | shuf -n 1)

	if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ] || [ -z "$TEST_PORT_3" ] || [ -z "$TEST_PORT_4" ]; then
		if [ -z "${pf}" ]; then
			pf=$("${mtl_folder}/script/nicctl.sh" list up | awk '$3 == "ice" {print $2}' | shuf -n 1)
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

	for dma_mechanism in "CBDMA" "idxd"; do
		TEST_DMA_PORT_P=$(dpdk-devbind.py -s | grep "$dma_mechanism" | awk '{print $1}' | shuf -n 1)
		TEST_DMA_PORT_R=$(dpdk-devbind.py -s | grep "$dma_mechanism" | grep -v "${TEST_DMA_PORT_P}" | awk '{print $1}' | shuf -n 1)
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
	sudo killall -SIGINT MtlManager >/dev/null 2>&1 || true
	sleep 2
}

start_mtl_manager() {
	if ! pgrep -f MtlManager >/dev/null; then
		echo "Starting MtlManager..."
		sudo MtlManager &
		sleep 3
	fi
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
			echo "✗ Test failed: $test_name (attempt $attempt/$MAX_RETRIES) | tee -a $LOG_FILE"

			kill_test_processes

			if [ $attempt -lt "$MAX_RETRIES" ]; then
				echo "Waiting $RETRY_DELAY seconds before retry..."
				sleep "$RETRY_DELAY"

				reset_ice_driver
				bind_driver_to_dpdk
			fi

			start_mtl_manager
			((attempt++))
		fi
	done

	echo "✗ Test failed after $MAX_RETRIES attempts: $test_name | tee -a $LOG_FILE"

	if [ "$EXIT_ON_FAILURE" -eq 1 ]; then
		echo "Exiting due to test failure."
		kill_test_processes
		time_taken_by_script
		exit 1
	fi
}

echo "Starting MTL test suite..."
echo "Maximum retries per test: $MAX_RETRIES"
echo "Retry delay: $RETRY_DELAY seconds"
echo "Exit on failure: $EXIT_ON_FAILURE"
echo "MTL_LD_PRELOAD path: $MTL_LD_PRELOAD"
echo "MUFD_CFG path: $MUFD_CFG"
echo ""

kill_test_processes
start_mtl_manager

failed_tests=()
passed_tests=()

if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ]; then
	bind_driver_to_dpdk
fi

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
		echo "✓ Test passed: $test_name"
		for i in "${passed_tests[@]}"; do
			echo " - $i"
		done
	elif [ $? -eq 2 ]; then
		echo "✗ Test aborted due to configuration errors: $test_name"
		kill_test_processes
		time_taken_by_script
		exit 1
	else
		failed_tests+=("$test_name")
		break
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
