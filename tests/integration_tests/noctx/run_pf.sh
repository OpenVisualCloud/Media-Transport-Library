#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Runs the NoCtxTest cases that require PF ports (name contains "_pf_", e.g.
# TSN/launch-time-pacing offload -- only advertised by PF drivers, not VF).
# run.sh excludes these from its VF-based full run; use this script to run
# just them, against a pair of ports bound as PF to vfio-pci.
#
# Most PF-only tests only need a single TX/RX pair, so 2 ports are enough.

: "${EXIT_ON_FAILURE:=1}"

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
# shellcheck disable=SC1091
. "${script_folder}/../../../script/common.sh"
cd "${script_folder}" || exit 1

mtl_folder="${script_folder}/../../.."

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

if [ -z "$TEST_PF_PORT_1" ] || [ -z "$TEST_PF_PORT_2" ]; then
	echo "Error: TEST_PF_PORT_1/TEST_PF_PORT_2 environment variables are not set"
	echo "These must be bound to vfio-pci as PFs, not VFs -- TSN/launch-time"
	echo "pacing offload is only advertised by PF drivers."
	echo "TEST_PF_PORT_1=$TEST_PF_PORT_1"
	echo "TEST_PF_PORT_2=$TEST_PF_PORT_2"
	exit 1
fi

PORT_LIST="${TEST_PF_PORT_1},${TEST_PF_PORT_2}"

test_names=$("$BUILD_PATH" --gtest_list_tests --no_ctx --port_list="${PORT_LIST}" --gtest_filter="NoCtxTest.${NOCTX_FILTER}*_pf_*" 2>/dev/null |
	awk '/^  [a-zA-Z]/ {gsub(/^  /, ""); print}')

if [ -z "$test_names" ]; then
	echo "No PF-only NoCtx tests found (none match *_pf_*)."
	exit 0
fi

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
	xml_file="${XML_OUTPUT_DIR}/noctx_pf_${test_count}.xml"

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

	sleep 10
done < <(echo "$test_names")

echo "All PF-only noctx tests completed. XML files saved in $XML_OUTPUT_DIR"
echo "Total test count: $test_count"
