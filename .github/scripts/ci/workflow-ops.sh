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
	# the main of the last nightly that passed and the main of this one.
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
	# The numbers are string inputs (perf-compare.yml says why), so the form
	# takes any text: stop a typo here, before the build.
	for v in SESSIONS TEST_TIME ROUNDS CORES; do
		[[ -z ${!v:-} || ${!v} =~ ^[1-9][0-9]*$ ]] || {
			echo "::error::${v,,} must be a positive number: ${!v@Q}" >&2
			exit 1
		}
	done
	# Each window of the case needs two whole stats dumps, 10 s apart.
	if ((${TEST_TIME:-30} < 30)); then
		echo "::error::test_time must be at least 30: ${TEST_TIME}" >&2
		exit 1
	fi
	# A slot's CPU for the application threads, one for a scheduler and one for
	# the system tasklets, unless both roles are single core.
	floor=3
	[[ ${SINGLE_CORE:-none} != both ]] || floor=2
	if ((${CORES:-3} < floor)); then
		echo "::error::cores must be at least ${floor}: ${CORES}" >&2
		exit 1
	fi
	# An MTL scheduler takes 60 video sessions at most.
	if [[ ${SINGLE_CORE:-none} != none ]] && ((${SESSIONS:-36} > 60)); then
		echo "::error::single_core takes up to 60 sessions a port: ${SESSIONS}" >&2
		exit 1
	fi
	# What a port of the case carries, at most 100G: the rate of pc.stream_bps(),
	# keep in step, 20-bit pixels paced over 1125 of 1080 lines, 1406 bytes for 1320
	# of pixels.
	video=${VIDEO:-1080p59}
	if ! awk -v n="${SESSIONS:-36}" -v h="${video%p*}" -v f="${video#*p}" 'BEGIN {
		fps[25] = 25; fps[29] = 30000 / 1001; fps[50] = 50; fps[59] = 60000 / 1001
		exit n * h * h * 16 / 9 * 20 * fps[f] * 1125 / 1080 * 1406 / 1320 > 100e9
	}'; then
		echo "::error::${SESSIONS:-36} x ${video} take more than a 100G port carries" >&2
		exit 1
	fi
	if [[ ${GITHUB_EVENT_NAME:-} == schedule ]]; then
		# GitHub starts each night hours late, by a different amount, so a window
		# counted back from the start would leave commits to no night. Without a
		# passed night, main a day ago.
		sha_b=${GITHUB_SHA:?GITHUB_SHA is required} name_b=main
		name_a='main of the last nightly'
		sha_a=$(gh api 'repos/{owner}/{repo}/actions/workflows/perf-compare.yml/runs?branch=main&event=schedule&status=success&per_page=1' \
			--jq '.workflow_runs[0].head_sha // empty') ||
			echo '::warning::The last passed nightly is unknown: the Actions API read failed' >&2
		# Not an ancestor of B when an older night is run again after a newer one passed.
		if [[ -z $sha_a ]] || ! git -C "$root_dir" merge-base --is-ancestor "$sha_a" "$sha_b"; then
			name_a='main a day ago'
			sha_a=$(git -C "$root_dir" rev-list -1 --first-parent --before='24 hours ago' origin/main)
		fi
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
