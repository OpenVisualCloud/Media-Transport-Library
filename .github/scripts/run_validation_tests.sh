#!/bin/bash

set +e

VALIDATION_TESTS_1="${1:-$VALIDATION_TESTS_1}"
VALIDATION_TESTS_2="${2:-$VALIDATION_TESTS_2}"
PYTEST_ALIAS="${3:-$PYTEST_ALIAS}"
PYTEST_PARAMS="${4:-$PYTEST_PARAMS}"
export TEST_PORT_P="${5:-$TEST_PORT_P}"
export TEST_PORT_R="${6:-$TEST_PORT_R}"
PYTEST_RETRIES="${PYTEST_RETRIES:-3}"

# Function to log messages to GitHub Actions
function LOG_GITHUB_SUMMARY() {
  echo "$@" >> "$GITHUB_STEP_SUMMARY"
}

function LOG_GITHUB_CONSOLE() {
  echo "$@"
}

# Function to run a test and handle retries
run_test() {
  local test=$1
  local retries=$2
  local pytest_alias=$3
  local pytest_params=$4
  local test_port_p=$5
  local test_port_r=$6
  local PYTEST_START_TIME=""
  local PYTEST_END_TIME=""
  local PYTEST_DURATION=""
  local PYTEST_TASK_STATUS="❌"
  local PYTEST_SUFFIX="[Err]"

  LOG_GITHUB_CONSOLE "::group::${test}"
  PYTEST_START_TIME=$(date '+%s')
  # shellcheck disable=SC2086
  ${pytest_alias} "${test}" ${pytest_params} --nic="${test_port_p},${test_port_r}" --collect-only -q --no-summary

  for retry in $(seq 1 "$retries"); do
    # shellcheck disable=SC2086
    ${pytest_alias} "${test}" ${pytest_params} --nic="${test_port_p},${test_port_r}"
    local result=$?
    LOG_GITHUB_CONSOLE "RETRY: ${retry}"
    [[ "$result" == "0" ]] && break
  done

  PYTEST_END_TIME="$(date '+%s')"
  PYTEST_DURATION="$((PYTEST_END_TIME - PYTEST_START_TIME))"

  if [[ "$result" == "0" ]]; then
    PYTEST_TASK_STATUS="✅"
    PYTEST_SUFFIX="[OK]"
    TESTS_SUCCESS+=("${test}")
  else
    TESTS_FAIL+=("${test}")
  fi

  LOG_GITHUB_SUMMARY "| ${PYTEST_TASK_STATUS} | ${test} | $(date --date=@${PYTEST_START_TIME} '+%d%m%y_%H%M%S') | $(date --date="@${PYTEST_END_TIME}" '+%d%m%y_%H%M%S') | ${PYTEST_DURATION}s | ${PYTEST_SUFFIX} |"
  LOG_GITHUB_CONSOLE "::endgroup::"
}

# Main script execution
LOG_GITHUB_CONSOLE "::group::pre-execution-summary"

# Collect tests to be executed
TESTS_INCLUDED_IN_EXECUTION=(
  $(grep -v "collected in" <(${PYTEST_ALIAS} "tests/${VALIDATION_TESTS_1}" --collect-only -q --no-summary 2>&1))
)
SUMMARY_MAIN_HEADER="Starting tests/${VALIDATION_TESTS_1}"

if [[ -n "${VALIDATION_TESTS_2}" ]]; then
  TESTS_INCLUDED_IN_EXECUTION+=(
    $(grep -v "collected in" <(${PYTEST_ALIAS} "tests/${VALIDATION_TESTS_2}" --collect-only -q --no-summary 2>&1))
  )
  SUMMARY_MAIN_HEADER="${SUMMARY_MAIN_HEADER}, tests/${VALIDATION_TESTS_2}"
fi

TESTS_FAIL=()
TESTS_SUCCESS=()

LOG_GITHUB_CONSOLE "${SUMMARY_MAIN_HEADER} tests (total ${NUMBER_OF_TESTS}) :rocket:"
LOG_GITHUB_CONSOLE "----------------------------------"
LOG_GITHUB_CONSOLE "Tests to be executed:"
LOG_GITHUB_CONSOLE "${TESTS_INCLUDED_IN_EXECUTION[@]}"

LOG_GITHUB_SUMMARY "## ${SUMMARY_MAIN_HEADER} tests (total ${NUMBER_OF_TESTS}) :rocket:"
LOG_GITHUB_SUMMARY "| ❌/✅ | Collected Test | Started | Ended | Took (s) | Result |"
LOG_GITHUB_SUMMARY "| --- | --- | --- | --- | --- | --- |"

LOG_GITHUB_CONSOLE "::endgroup::"

# Execute each test
for test in "${TESTS_INCLUDED_IN_EXECUTION[@]}"; do
  run_test "$test" "${PYTEST_RETRIES}" "${PYTEST_ALIAS}" "${PYTEST_PARAMS}" "${TEST_PORT_P}" "${TEST_PORT_R}"
done

# Summary of test results
LOG_GITHUB_SUMMARY "### Total success ${#TESTS_SUCCESS[@]}/${NUMBER_OF_TESTS}:"
LOG_GITHUB_SUMMARY "${TESTS_SUCCESS[@]}"
LOG_GITHUB_SUMMARY "### Total failed ${#TESTS_FAIL[@]}/${NUMBER_OF_TESTS}:"
LOG_GITHUB_SUMMARY "${TESTS_FAIL[@]}"

# Determine exit status
if [[ "${#TESTS_FAIL[@]}" == "0" ]] || [[ "${VALIDATION_NO_FAIL_TESTS}" == "true" ]]; then
  exit 0
fi

exit 1
