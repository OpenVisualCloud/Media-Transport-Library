#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"
cd "${root_dir}/tests/acceptance"
report=$PWD/report.html

case "${1:-}" in
custom)
	args=("./${TEST_PATH:?TEST_PATH is required}")
	[[ -n ${PYTEST_MARKER:-} ]] && args+=(-m "$PYTEST_MARKER")
	[[ -n ${PYTEST_FILTER:-} ]] && args+=(-k "$PYTEST_FILTER")
	;;
performance)
	args=(./tests/dual/performance -k "${PYTEST_FILTER:-1080p and 59fps}")
	[[ -n ${PYTEST_MARKER:-} ]] && args+=(-m "$PYTEST_MARKER")
	[[ -n ${NUM_SESSIONS:-} ]] && args+=(--num_sessions "$NUM_SESSIONS")
	[[ -n ${SCH_QUOTA:-} ]] && args+=(--sch_quota "$SCH_QUOTA")
	;;
nightly) args=("./tests/single/${TEST_PATH:?TEST_PATH is required}" -m nightly) ;;
smoke | smoke-low-bandwidth)
	# The upload step and reports.sh read the reports from the workspace root.
	report=${root_dir}/report.html
	args=(./tests --json-report --json-report-file="${root_dir}/report.json")
	# low_bandwidth fits a 2.5 GbE (i225/i226) link and is not fail-fast, so one failure hides nothing.
	if [[ $1 == smoke ]]; then args+=(-m smoke -x); else args+=(-m low_bandwidth); fi
	;;
*)
	echo "Usage: $0 {custom|performance|nightly|smoke|smoke-low-bandwidth}" >&2
	exit 2
	;;
esac

# shellcheck disable=SC2154 # set by mtl_acceptance_venv.sh
"${venv_python}" -m pytest --test_config="$PWD/configs/test_config.yaml" \
	--topology_config="$PWD/configs/topology_config.yaml" \
	--template=html/index.html --report="$report" "${args[@]}"
