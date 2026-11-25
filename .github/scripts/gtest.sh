#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Thin CI wrapper forwarding env vars to tests/integration_tests/runner/gtest_runner.py.

set -euo pipefail

mtl_folder="$(realpath "$(dirname "${BASH_SOURCE[0]}")/../..")"
args=(
	--repo-root "${mtl_folder}"
	--out-dir "${GTEST_OUT_DIR:-${TMP_FOLDER:-/tmp/mtl_gtest_$(date +%Y%m%d_%H%M%S)_$$}}"
	--retries "${MAX_RETRIES:-2}"
	--retry-delay "${RETRY_DELAY:-5}"
	--timeout "${TEST_CASE_TIMEOUT:-1800}"
	--noctx-timeout "${NOCTX_CASE_TIMEOUT:-2100}"
	--noctx-sleep "${NOCTX_SLEEP:-0}"
)
[ "${NIGHTLY:-1}" -eq 1 ] && args+=(--tier nightly) || args+=(--tier pr --fail-fast)
[ "${EXIT_ON_FAILURE:-1}" -eq 1 ] && args+=(--exit-on-failure)
[ -n "${GTEST_GROUP:-}" ] && args+=(--group "${GTEST_GROUP}")
[ -n "${GTEST_CASES:-}" ] && args+=(--profile "${GTEST_CASES}")
[ -n "${LOG_FILE:-}" ] && args+=(--log-file "${LOG_FILE}")
[ -n "${KAHAWAI_TEST_BINARY:-}" ] && args+=(--binary "${KAHAWAI_TEST_BINARY}")
[ -n "${TEST_P_SIP:-}" ] && args+=(--p-sip "${TEST_P_SIP}")
[ -n "${TVF_PFS:-}" ] && args+=(--tvf-pf "${TVF_PFS}")
[ -n "${BIND_PVFS:-}" ] && args+=(--bind-pvf "${BIND_PVFS}")

exec python3 "${mtl_folder}/tests/integration_tests/runner/gtest_runner.py" "${args[@]}" "$@"
