#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

args=(--check --mode "${MODE:-all}")
[[ ${STRICT:-true} == true ]] && args+=(--strict)
[[ ${REQUIRE_XDP:-false} == true ]] && args+=(--require-xdp)
bash "${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required}/script/build_ebpf_xdp.sh" "${args[@]}"