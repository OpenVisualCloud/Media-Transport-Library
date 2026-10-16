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
perf-compare-refs)
	# The commits perf-compare.yml builds: the dispatched refs, or for the nightly
	# main as it was a day before this run started and main now.
	resolve() {
		# A name, a SHA or a ~ or ^ suffix; no option, no ^{/regex} and no newline,
		# which would add a line to GITHUB_OUTPUT.
		[[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._/~^-]*$ ]] || {
			echo "::error::Not a branch, tag or SHA: ${1@Q}" >&2
			return 1
		}
		git -C "$root_dir" rev-parse --verify --quiet "origin/$1^{commit}" ||
			git -C "$root_dir" rev-parse --verify "$1^{commit}"
	}
	if [[ ${GITHUB_EVENT_NAME:-} == schedule ]]; then
		sha_a=$(git -C "$root_dir" rev-list -1 --first-parent --before='24 hours ago' origin/main)
		name_a='main a day ago' sha_b=$(resolve main) name_b=main
	else
		sha_a=$(resolve "${REF_A:?REF_A is required}") name_a=$REF_A
		sha_b=$(resolve "${REF_B:?REF_B is required}") name_b=$REF_B
	fi
	{
		echo "sha_a=${sha_a}"
		echo "sha_b=${sha_b}"
		echo "label_a=${name_a} ${sha_a:0:9}"
		echo "label_b=${name_b} ${sha_b:0:9}"
	} | tee -a "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
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
	echo "Usage: $0 {overlay-tests|check-built-sources|perf-compare-refs|both-workflows|docs-dependencies|coverity-dependencies|coverity-dpdk}" >&2
	exit 2
	;;
esac
