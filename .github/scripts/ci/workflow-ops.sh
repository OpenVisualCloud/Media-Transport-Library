#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail
cd "$(dirname "$0")/../../.."

case "${1:-}" in
overlay-tests)
	echo "MTL source: ${MTL_SOURCE}"
	echo "Test framework: ${TEST_SHA} (${TEST_REF})"
	git checkout "$TEST_SHA" -- tests/acceptance/ .github/ Taskfile.yml script/hash_sources.sh 'script/hash_sources_*.env'
	;;
check-built-sources)
	field() {
		jq -er --arg key "$2" '.[$key] | select(. != "")' <<<"$1" || {
			echo "::error::$2 is missing" >&2
			exit 1
		}
	}
	commit=$(field "$BUILD_OUTPUTS" mtl_commit)
	echo "Built commit: ${commit}, checked out: $(git rev-parse HEAD)"
	status=0
	for component in mtl jpegxs ice ffmpeg gstreamer plugins; do
		built=$(field "$BUILD_OUTPUTS" "${component}_hash")
		checkout=$(field "$CHECKSUMS" "$component")
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
	both=false
	[[ -n ${GTEST_RUN_ID:-} && -n ${PYTEST_RUN_ID:-} ]] && both=true
	echo "both_completed=${both}" >>"$GITHUB_OUTPUT"
	[[ $both == true ]] || { echo 'Waiting for both workflows to complete...' && exit 1; }
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
	cd script && ./build_dpdk.sh
	;;
*)
	echo "Usage: $0 {overlay-tests|check-built-sources|both-workflows|docs-dependencies|coverity-dependencies|coverity-dpdk}" >&2
	exit 2
	;;
esac
