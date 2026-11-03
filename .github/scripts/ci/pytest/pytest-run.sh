#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
# shellcheck source-path=SCRIPTDIR source=../../lib/mtl_acceptance_venv.sh disable=SC1091
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
	# Joined with '=': the value starts with '--', which pytest would otherwise
	# read as an option of its own.
	[[ -n ${SUT_APP_ARGS:-} ]] && args+=("--sut_app_args=$SUT_APP_ARGS")
	;;
perf-compare)
	video=${VIDEO:-1080p59}
	# a nics or pixel_format choice of the workflow reads "<word>: what it does"
	nics=${NICS:-single} pixel_format=${PIXEL_FORMAT:-none}
	args=(./tests/dual/performance/test_perf_compare.py -k "${video%p*}p and ${video##*p}fps"
		--perf_compare_slots="${PERF_COMPARE_SLOTS:?PERF_COMPARE_SLOTS is required}"
		--perf_compare_labels="${PERF_COMPARE_LABELS:-A,B}"
		--perf_compare_args_a="${ARGS_A:-}" --perf_compare_args_b="${ARGS_B:-}"
		--perf_compare_dma="${DMA:-none}" --perf_compare_nics="${nics%%:*}"
		--perf_compare_pixel_format="${pixel_format%%:*}")
	[[ -n ${SESSIONS:-} ]] && args+=(--perf_compare_sessions "$SESSIONS")
	[[ -n ${ROUNDS:-} ]] && args+=(--perf_compare_rounds "$ROUNDS")
	# booleans: "true" or "false", or empty in a run without inputs
	[[ ${SINGLE_CORE_RX:-} == true ]] && args+=(--perf_compare_single_core_rx)
	[[ ${SINGLE_CORE_TX:-} == true ]] && args+=(--perf_compare_single_core_tx)
	;;
nightly)
	args=("./tests/single/${TEST_PATH:?TEST_PATH is required}" -m nightly)
	[[ -n ${PYTEST_FILTER:-} ]] && args+=(-k "$PYTEST_FILTER")
	[[ -n ${PYTEST_SHARD:-} ]] && args+=(--shard "$PYTEST_SHARD")
	;;
smoke | smoke-low-bandwidth)
	# The upload step and reports.sh read the reports from the workspace root.
	report=${root_dir}/report.html
	args=(./tests --json-report --json-report-file="${root_dir}/report.json")
	# low_bandwidth fits a 2.5 GbE (i225/i226) link and is not fail-fast, so one failure hides nothing.
	if [[ $1 == smoke ]]; then args+=(-m smoke -x); else args+=(-m low_bandwidth); fi
	;;
*)
	echo "Usage: $0 {custom|performance|perf-compare|nightly|smoke|smoke-low-bandwidth}" >&2
	exit 2
	;;
esac

# shellcheck disable=SC2154 # set by mtl_acceptance_venv.sh
"${venv_python}" -m pytest --test_config="$PWD/configs/test_config.yaml" \
	--topology_config="$PWD/configs/topology_config.yaml" \
	--template=html/index.html --report="$report" "${args[@]}"
