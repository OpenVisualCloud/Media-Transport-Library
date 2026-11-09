#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail
cd "$(dirname "$0")/../../../.."

case "${1:-}" in
overlay-tests)
	echo "MTL source: ${MTL_SOURCE}"
	echo "Test framework: ${TEST_SHA} (${TEST_REF})"
	# The overlaid CI and tests call script/ and plugin interfaces that older refs lack.
	grep -q MTL_FLAG_DMA_AUTO include/mtl_api.h || {
		echo "::error::${MTL_SOURCE} is too old for the overlay: it has no MTL_FLAG_DMA_AUTO" >&2
		exit 1
	}
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
		git rev-parse --verify --quiet "origin/$1^{commit}" ||
			git rev-parse --verify "$1^{commit}"
	}
	# The numbers are string inputs (perf-compare.yml says why), so the form
	# takes any text: stop a typo here, before the build.
	for v in SESSIONS TEST_TIME ROUNDS; do
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
	# The sessions of a build, which whole puts on two ports, filling the
	# first before the second (pc.port_sessions()). A port carries at most 100G: the
	# rate of pc.stream_bps(), keep in step, 20-bit pixels paced over 1125 of 1080
	# lines, 1406 bytes for 1320 of pixels. A nics choice reads "<word>: what it does".
	video=${VIDEO:-1080p59} sessions=${SESSIONS:-36} nics=${NICS:-single}
	# st22p with JPEG XS, which test_perf_compare.py refuses alike, and says why
	codec=${CODEC:-none}
	refuse() {
		echo "::error::codec jpegxs takes $1" >&2
		exit 1
	}
	if [[ ${codec%%:*} == jpegxs ]]; then
		((sessions <= 8)) || refuse "up to 8 sessions, as many as the SVT-JPEG-XS plugin encodes or decodes an instance: ${sessions}"
		[[ ${video%p*} == 1080 ]] || refuse "1080p: no other resolution has a planar clip short enough for each TX session's hugepages"
		[[ ${nics%%:*} != whole ]] || refuse "no nics whole: one port carries its sessions, so split"
		[[ ${DMA:-none} == none ]] || refuse "no dma: MTL's st22 RX copies with none"
		[[ ${PIXEL_FORMAT:-none} == none* ]] || refuse "no pixel_format: its encoder takes the planar clip as it is"
		[[ ${COMPLIANCE:-} != true ]] || refuse "no compliance: EBU LIST takes a -22 stream's media type from its SDP"
	fi
	most=$(awk -v h="${video%p*}" -v f="${video#*p}" 'BEGIN {
		fps[25] = 25; fps[29] = 30000 / 1001; fps[50] = 50; fps[59] = 60000 / 1001
		print int(100e9 / (h * h * 16 / 9 * 20 * fps[f] * 1125 / 1080 * 1406 / 1320))
	}')
	if [[ ${nics%%:*} == whole ]]; then
		if ((sessions > 2 * most)); then
			echo "::error::${sessions} x ${video} take more than two 100G ports carry, $((2 * most))" >&2
			exit 1
		fi
	elif ((sessions > most)); then
		echo "::error::${sessions} x ${video} take more than a 100G port carries, ${most}; nics whole puts a build on two, up to $((2 * most))" >&2
		exit 1
	fi
	# An MTL scheduler takes 60 video sessions at most. Booleans: "true" or "false",
	# or empty in a run without inputs.
	if [[ ${SINGLE_CORE_RX:-} == true || ${SINGLE_CORE_TX:-} == true ]] && ((sessions > 60)); then
		echo "::error::single core takes up to 60 sessions, as many as an MTL scheduler: ${sessions}" >&2
		exit 1
	fi
	if [[ ${COMPLIANCE:-} == true && ${video%p*} == 4320 ]]; then
		echo "::error::compliance: EBU LIST cannot judge ${video}" >&2
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
		if [[ -z $sha_a ]] || ! git merge-base --is-ancestor "$sha_a" "$sha_b"; then
			name_a='main a day ago'
			sha_a=$(git rev-list -1 --first-parent --before='24 hours ago' origin/main)
		fi
	else
		sha_a=$(resolve "${REF_A:?REF_A is required}") name_a=$REF_A
		sha_b=$(resolve "${REF_B:?REF_B is required}") name_b=$REF_B
	fi
	# The SVT-JPEG-XS plugin of versions.env asks for MTL 26.09 or later (its
	# mtl-plugin/meson.build), which meson reads from VERSION: keep in step.
	if [[ ${codec%%:*} == jpegxs ]]; then
		for sha in "$sha_a" "$sha_b"; do
			version=$(git show "${sha}:VERSION" 2>/dev/null) || version=0.0
			IFS=. read -r major minor _ <<<"$version"
			((10#$major * 100 + 10#$minor >= 2609)) ||
				refuse "builds of MTL 26.09 or later, as its SVT-JPEG-XS plugin asks: ${sha:0:9} is ${version}"
		done
	fi
	{
		echo "sha_a=${sha_a}"
		echo "sha_b=${sha_b}"
		echo "label_a=${name_a} ${sha_a:0:9}"
		echo "label_b=${name_b} ${sha_b:0:9}"
	} | tee -a "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
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
	echo "Usage: $0 {overlay-tests|check-built-sources|perf-compare-refs|both-workflows|docs-dependencies|coverity-dependencies|coverity-dpdk}" >&2
	exit 2
	;;
esac
