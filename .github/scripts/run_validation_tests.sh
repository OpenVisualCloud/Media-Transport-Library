#!/bin/bash

set +e

# Function to log messages to GitHub Actions
log_to_github() {
	echo "$1" >>"$GITHUB_STEP_SUMMARY"
}

# Function to run a test and handle retries
run_test() {
	local test=$1
	local retries=$2
	local test_port_p=$3
	local test_port_r=$4
	local start_time
	local end_time
	local duration

	echo "::group::${test}"
	start_time=$(date '+%s')
	sudo --preserve-env python3 -m pipenv run pytest "${test}" --media=/mnt/media --build="../.." --nic="${test_port_p},${test_port_r}" --collect-only -q --no-summary

	for retry in $(seq 1 "$retries"); do
		echo "sudo --preserve-env python3 -m pipenv run pytest \"${test}\" --media=/mnt/media --build=\"../..\" --nic=\"${test_port_p},${test_port_r}\""
		sudo --preserve-env python3 -m pipenv run pytest "${test}" --media=/mnt/media --build="../.." --nic="${test_port_p},${test_port_r}"
		local result=$?
		echo "RETRY: ${retry}"
		[[ "$result" == "0" ]] && break
	done

	end_time=$(date '+%s')
	duration=$((end_time - start_time))
	local status="❌"
	local suffix="[Err]"

	if [[ "$result" == "0" ]]; then
		status="✅"
		suffix="[OK]"
		TESTS_SUCCESS+=("${test}")
	else
		TESTS_FAIL+=("${test}")
	fi

	log_to_github "| ${status} | ${test} | $(date --date="@${start_time}" '+%d%m%y_%H%M%S') | $(date --date="@${end_time}" '+%d%m%y_%H%M%S') | ${duration}s | ${suffix} |"
	echo "::endgroup::"
}

# Main script execution
echo "::group::pre-execution-summary"

# Export environment variables
export TEST_PORT_P="${TEST_PORT_P}"
export TEST_PORT_R="${TEST_PORT_R}"

SUMMARY_MAIN_HEADER="Starting "
# Collect tests to be executed
if [[ -n "${VALIDATION_TESTS_1}" ]]; then
	SUMMARY_MAIN_HEADER="${SUMMARY_MAIN_HEADER} tests/${VALIDATION_TESTS_1}"
	python3 -m pipenv run pytest "tests/${VALIDATION_TESTS_1}" --media=/mnt/media --build="../.." --nic="${TEST_PORT_P},${TEST_PORT_R}" --collect-only -q --no-summary >tests.log 2>&1
fi

if [[ -n "${VALIDATION_TESTS_2}" ]]; then
	SUMMARY_MAIN_HEADER="${SUMMARY_MAIN_HEADER}, tests/${VALIDATION_TESTS_2}"
	python3 -m pipenv run pytest "tests/${VALIDATION_TESTS_2}" --media=/mnt/media --build="../.." --nic="${TEST_PORT_P},${TEST_PORT_R}" --collect-only -q --no-summary >>tests.log 2>&1
fi

mapfile -t TESTS_INCLUDED_IN_EXECUTION < <(grep -v "collected in" tests.log)
NUMBER_OF_TESTS="${#TESTS_INCLUDED_IN_EXECUTION[@]}"
TESTS_FAIL=()
TESTS_SUCCESS=()

echo "${SUMMARY_MAIN_HEADER} tests (total ${NUMBER_OF_TESTS}) :rocket:"
echo "----------------------------------"
echo "Tests to be executed:"
echo "${TESTS_INCLUDED_IN_EXECUTION[@]}"

log_to_github "## ${SUMMARY_MAIN_HEADER} tests (total ${NUMBER_OF_TESTS}) :rocket:"
log_to_github "| ❌/✅ | Collected Test | Started | Ended | Took (s) | Result |"
log_to_github "| --- | --- | --- | --- | --- | --- |"
echo "::endgroup::"

# Execute each test
for test in "${TESTS_INCLUDED_IN_EXECUTION[@]}"; do
	run_test "$test" "$PYTEST_RETRIES" "$TEST_PORT_P" "$TEST_PORT_R"
done

# Summary of test results
log_to_github "### Total success ${#TESTS_SUCCESS[@]}/${NUMBER_OF_TESTS}:"
log_to_github "${TESTS_SUCCESS[@]}"
log_to_github "### Total failed ${#TESTS_FAIL[@]}/${NUMBER_OF_TESTS}:"
log_to_github "${TESTS_FAIL[@]}"

# Determine exit status
if [[ "${#TESTS_FAIL[@]}" == "0" ]] || [[ "${VALIDATION_NO_FAIL_TESTS}" == "true" ]]; then
	exit 0
else
	exit 1
fi
