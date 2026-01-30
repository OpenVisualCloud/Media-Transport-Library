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

# Create temporary directory for XML files
TEMP_XML_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_XML_DIR"' EXIT

set -e
test_count=0
while IFS= read -r test_name || [ -n "$test_name" ]; do
	if [[ -z "$test_name" || "$test_name" == \#* ]]; then
		continue
	fi
	echo "Checking test: NoCtxTest.$test_name"

	test_count=$((test_count + 1))
	xml_file="${TEMP_XML_DIR}/noctx_${test_count}.xml"

	if "$BUILD_PATH" \
		--auto_start_stop \
		--port_list="${TEST_PORT_1},${TEST_PORT_2},${TEST_PORT_3},${TEST_PORT_4}" \
		--gtest_filter="NoCtxTest.$test_name" \
		--gtest_output="xml:${xml_file}" \
		--no_ctx_tests; then
		echo "Test NoCtxTest.$test_name passed"
	else
		echo "Test NoCtxTest.$test_name failed with exit code $?"
		exit 1
	fi

	sleep 30
done < <(echo "$test_names")

# Merge all XML files into a single combined XML
OUTPUT_XML="${OUTPUT_XML:-/tmp/gtest_noctx.xml}"
echo "Merging XML results to $OUTPUT_XML"

# Create merged XML with proper structure
echo '<?xml version="1.0" encoding="UTF-8"?>' >"$OUTPUT_XML"
echo '<testsuites tests="0" failures="0" disabled="0" errors="0" time="0" name="NoCtxTests">' >>"$OUTPUT_XML"

total_tests=0
total_failures=0
total_time=0

for xml_file in "$TEMP_XML_DIR"/noctx_*.xml; do
	if [ -f "$xml_file" ]; then
		# Extract testsuite content (skip XML declaration and testsuites wrapper)
		# Use word boundaries to avoid matching <testsuites> when looking for <testsuite>
		sed -n '/<testsuite /,/<\/testsuite>/p' "$xml_file" >>"$OUTPUT_XML"

		# Extract counts
		tests=$(grep -oP 'tests="\K[0-9]+' "$xml_file" | head -1 || echo "0")
		failures=$(grep -oP 'failures="\K[0-9]+' "$xml_file" | head -1 || echo "0")
		time=$(grep -oP 'time="\K[0-9.]+' "$xml_file" | head -1 || echo "0")

		total_tests=$((total_tests + tests))
		total_failures=$((total_failures + failures))
		total_time=$(awk "BEGIN {print $total_time + $time}")
	fi
done

echo '</testsuites>' >>"$OUTPUT_XML"

# Update the merged XML header with correct totals
sed -i "s/tests=\"0\"/tests=\"$total_tests\"/" "$OUTPUT_XML"
sed -i "s/failures=\"0\"/failures=\"$total_failures\"/" "$OUTPUT_XML"
sed -i "s/time=\"0\"/time=\"$total_time\"/" "$OUTPUT_XML"

echo "Combined XML created with $total_tests tests ($total_failures failures)"
