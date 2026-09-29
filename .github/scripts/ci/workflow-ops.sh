#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

case "${1:-}" in
overlay-tests)
	echo "MTL source: ${MTL_SOURCE:?MTL_SOURCE is required}"
	echo "Test framework: ${TEST_SHA:?TEST_SHA is required} (${TEST_REF:?TEST_REF is required})"
	# The hash script and lists too: they decide which of these CI files the
	# cache keys cover, and only the workflow commit's lists name all of them.
	git -C "$root_dir" checkout "$TEST_SHA" -- tests/acceptance/ .github/ Taskfile.yml \
		script/hash_sources.sh 'script/hash_sources_*.env'
	;;
check-built-sources)
	# BUILD_OUTPUTS is toJSON(needs.build.outputs) of the build.yml call,
	# CHECKSUMS the source-checksums outputs of this checkout, as JSON. DPDK is
	# not compared: build.yml does not export it and the MTL checksum includes it.
	field() {
		jq -er --arg key "$2" '.[$key] | select(. != "")' <<<"$1" || {
			echo "::error::${2} is missing from ${3}" >&2
			exit 1
		}
	}
	commit=$(field "${BUILD_OUTPUTS:?BUILD_OUTPUTS is required}" mtl_commit 'the build outputs')
	echo "Built commit: ${commit}, checked out: $(git -C "$root_dir" rev-parse HEAD)"
	status=0
	for component in mtl jpegxs ice ffmpeg gstreamer plugins; do
		built=$(field "$BUILD_OUTPUTS" "${component}_hash" 'the build outputs')
		checkout=$(field "${CHECKSUMS:?CHECKSUMS is required}" "$component" 'the checkout checksums')
		if [[ $built == "$checkout" ]]; then
			echo "${component}: ${checkout}"
		else
			echo "::error::${component} checksum ${checkout} is not the ${built} the build job built: this job would test other code than was built"
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
	sudo apt-get install -y --no-install-recommends make python3 python3-venv
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
