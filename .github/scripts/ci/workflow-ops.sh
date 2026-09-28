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
check-built-sources)
	# BUILT_* are the checksums build.yml hashed and built, HASH_* those of
	# this checkout. DPDK is not listed: the MTL checksum includes it.
	echo "Built commit: ${BUILT_COMMIT:?BUILT_COMMIT is required}, checked out: $(git -C "$root_dir" rev-parse HEAD)"
	status=0
	for component in MTL JPEGXS ICE FFMPEG GSTREAMER PLUGINS; do
		built="BUILT_${component}" checkout="HASH_${component}"
		if [[ ${!built:?${built} is required} == "${!checkout:?${checkout} is required}" ]]; then
			echo "${component}: ${!checkout}"
		else
			echo "::error::${component} checksum ${!checkout} is not the ${!built} the build job built: this job would test other code than was built"
			status=1
		fi
	done
	exit "$status"
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
	echo "Usage: $0 {overlay-tests|check-built-sources|both-workflows|docs-dependencies|coverity-dependencies|coverity-dpdk}" >&2
	exit 2
	;;
esac
