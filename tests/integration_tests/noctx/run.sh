#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

: "${EXIT_ON_FAILURE:=1}"

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
# shellcheck disable=SC1091
. "${script_folder}/../../../script/common.sh"
cd "${script_folder}" || exit 1

mtl_folder="${script_folder}/../../.."
# time between tests is added due to DPDK driver reinitialization occasionally failing.
sleep_time=20

# Detect whether to use .local_install (CI) or local build paths
if [ -z "${BUILD_PATH:-}" ]; then
	if [ -d "${mtl_folder}/.local_install" ]; then
		BUILD_PATH="${mtl_folder}/.local_install/mtl/bin/KahawaiTest"
	else
		BUILD_PATH="${mtl_folder}/build/tests/KahawaiTest"
	fi
fi
ENV_FILE="${script_folder}/noctx.env"

if [ -f "$ENV_FILE" ]; then
	# shellcheck disable=SC1090
	. "$ENV_FILE"
fi

if [ ! -f "$BUILD_PATH" ]; then
	echo "Error: KahawaiTest binary not found at $BUILD_PATH"
	echo "Please build the project first"
	exit 1
fi

if [ -z "$TEST_PORT_1" ] || [ -z "$TEST_PORT_2" ] || [ -z "$TEST_PORT_3" ] || [ -z "$TEST_PORT_4" ]; then
	echo "Error: One or more TEST_PORT_X environment variables are not set"
	echo "TEST_PORT_1=$TEST_PORT_1"
	echo "TEST_PORT_2=$TEST_PORT_2"
	echo "TEST_PORT_3=$TEST_PORT_3"
	echo "TEST_PORT_4=$TEST_PORT_4"
	exit 1
fi

PORT_LIST="${TEST_PORT_1},${TEST_PORT_2},${TEST_PORT_3},${TEST_PORT_4}"

# PF-only tests (name contains "_pf_", e.g. TSN/launch-time-pacing) can
# never pass against these VF ports; exclude them explicitly instead of
# letting them fail for the wrong reason. Run them via run_pf.sh instead.
GTEST_FILTER="NoCtxTest.${NOCTX_FILTER}*-NoCtxTest.*_pf_*"

test_names=$("$BUILD_PATH" --gtest_list_tests --no_ctx --port_list="${PORT_LIST}" --gtest_filter="${GTEST_FILTER}" 2>/dev/null |
	awk '/^  [a-zA-Z]/ {gsub(/^  /, ""); print}')

# Use TMP_FOLDER from environment or fallback to /tmp
: "${TMP_FOLDER:=/tmp}"
XML_OUTPUT_DIR="${TMP_FOLDER}"
mkdir -p "$XML_OUTPUT_DIR"

test_count=0
while IFS= read -r test_name || [ -n "$test_name" ]; do
	if [[ -z "$test_name" || "$test_name" == \#* ]]; then
		continue
	fi
	echo "Checking test: NoCtxTest.$test_name"

	test_count=$((test_count + 1))
	xml_file="${XML_OUTPUT_DIR}/noctx_${test_count}.xml"

	if "$BUILD_PATH" \
		--auto_start_stop \
		--port_list="${PORT_LIST}" \
		--gtest_filter="NoCtxTest.$test_name" \
		--gtest_output="xml:${xml_file}" \
		--no_ctx_tests; then
		echo "Test NoCtxTest.$test_name passed"
	else
		echo "Test NoCtxTest.$test_name failed with exit code $?"
		if [ "$EXIT_ON_FAILURE" -eq 1 ]; then
			echo "Exiting due to test failure."
			exit 1
		fi
	fi

	echo -n "Waiting ${sleep_time}s "
	for ((i = sleep_time; i > 0; i--)); do
		printf '.'
		sleep 1
	done
	echo
done < <(echo "$test_names")

echo "All noctx tests completed. XML files saved in $XML_OUTPUT_DIR"
echo "Total test count: $test_count"
