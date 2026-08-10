#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

case "${1:-}" in
overlay-tests)
	echo "MTL source: ${MTL_SOURCE:?MTL_SOURCE is required}"
	echo "Test framework: ${TEST_SHA:?TEST_SHA is required} (${TEST_REF:?TEST_REF is required})"
	git -C "$root_dir" checkout "$TEST_SHA" -- tests/acceptance/ .github/ Taskfile.yml
	;;
both-workflows)
	if [[ -n ${GTEST_RUN_ID:-} && -n ${PYTEST_RUN_ID:-} ]]; then
		echo 'both_completed=true' >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
		echo 'Both workflows have completed runs available'
	else
		echo 'both_completed=false' >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
		echo 'Waiting for both workflows to complete...'
		exit 1
	fi
	;;
upstream-sync-failed)
	echo 'Something wrong, please sync upstream manually once.'
	exit 1
	;;
docs-dependencies)
	sudo apt-get update -y
	sudo apt-get install -y --no-install-recommends make python3 python3-pip python3-sphinx
	;;
coverity-dependencies)
	sudo apt-get update -y
	sudo apt-get install -y --no-install-recommends git build-essential meson python3 \
		python3-pyelftools pkg-config libnuma-dev libjson-c-dev libpcap-dev \
		libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev ca-certificates m4 \
		clang llvm zlib1g-dev libelf-dev libcap-ng-dev libcap2-bin gcc-multilib \
		systemtap-sdt-dev ninja-build nasm wget unzip
	sudo apt-get clean
	sudo rm -rf /var/lib/apt/lists/*
	;;
coverity-dpdk)
	(cd "${root_dir}/script" && ./build_dpdk.sh)
	;;
*)
	echo "Usage: $0 {overlay-tests|both-workflows|upstream-sync-failed|docs-dependencies|coverity-dependencies|coverity-dpdk}" >&2
	exit 2
	;;
esac