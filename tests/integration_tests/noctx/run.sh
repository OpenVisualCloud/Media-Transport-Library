#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
# shellcheck disable=SC1091
. "${script_folder}/../../../script/common.sh"
cd "${script_folder}" || exit 1

BUILD_PATH="${script_folder}/../../../build/tests/KahawaiTest"
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

test_names=$("$BUILD_PATH" --gtest_list_tests --no_ctx --port_list="${TEST_PORT_1},${TEST_PORT_2},${TEST_PORT_3},${TEST_PORT_4}" --gtest_filter="NoCtxTest.*" 2>/dev/null |
	awk '/^  [a-zA-Z]/ {gsub(/^  /, ""); print}')

set -e
while IFS= read -r test_name || [ -n "$test_name" ]; do
	if [[ -z "$test_name" || "$test_name" == \#* ]]; then
		continue
	fi
	echo "Checking test: NoCtxTest.$test_name"

	if "$BUILD_PATH" \
		--auto_start_stop \
		--port_list="${TEST_PORT_1},${TEST_PORT_2},${TEST_PORT_3},${TEST_PORT_4}" \
		--gtest_filter="NoCtxTest.$test_name" \
		--no_ctx_tests; then
		echo "Test NoCtxTest.$test_name passed"
	else
		echo "Test NoCtxTestTest.$test_name failed with exit code $?"
		exit 1
	fi

	sleep 30
done < <(echo "$test_names")
