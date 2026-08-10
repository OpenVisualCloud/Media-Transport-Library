#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"
pytest=("${acceptance_dir}/.venv/bin/python3" -m pytest
	--test_config="${acceptance_dir}/configs/test_config.yaml"
	--topology_config="${acceptance_dir}/configs/topology_config.yaml")

case "${1:-}" in
custom)
	args=(--template=html/index.html --report="${acceptance_dir}/report.html")
	[[ -n ${PYTEST_MARKER:-} ]] && args+=(-m "$PYTEST_MARKER")
	[[ -n ${PYTEST_FILTER:-} ]] && args+=(-k "$PYTEST_FILTER")
	(cd "$acceptance_dir" && "${pytest[@]}" "${args[@]}" "./${TEST_PATH:?TEST_PATH is required}")
	;;
performance)
	args=(-k "${PYTEST_FILTER:-1080p and 59fps}" --template=html/index.html
		--report="${acceptance_dir}/report.html")
	[[ -n ${PYTEST_MARKER:-} ]] && args+=(-m "$PYTEST_MARKER")
	[[ -n ${NUM_SESSIONS:-} ]] && args+=(--num_sessions "$NUM_SESSIONS")
	[[ -n ${SCH_QUOTA:-} ]] && args+=(--sch_quota "$SCH_QUOTA")
	(cd "$acceptance_dir" && "${pytest[@]}" "${args[@]}" ./tests/dual/performance)
	;;
nightly)
	(cd "$acceptance_dir" && "${pytest[@]}" -m nightly --template=html/index.html \
		--report=report.html "./tests/single/${TEST_PATH:?TEST_PATH is required}")
	;;
smoke)
	"${pytest[@]}" -m smoke -x --template=html/index.html --report="${root_dir}/report.html"
	;;
*)
	echo "Usage: $0 {custom|performance|nightly|smoke}" >&2
	exit 2
	;;
esac