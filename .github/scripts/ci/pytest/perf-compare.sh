#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Host side of perf-compare.yml (doc/perf_compare.md).

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
# Beside the workspace: checkout-clean empties it, and the companion's runner
# empties its RUNNER_TEMP whenever it starts a job.
slots=$(dirname "$root_dir")/perf-compare
prefix="${root_dir}/.local_install"
ssh_options=(-o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=30)

companion() {
	# shellcheck disable=SC2029
	ssh "${ssh_options[@]}" "${SHADOW_HOST_USER:?SHADOW_HOST_USER is required}@${SHADOW_HOST_IP:?SHADOW_HOST_IP is required}" "$@"
}

# The units, CAT groups and saved schemata of tests/acceptance/mtl_engine/perf_compare.py.
sweep() {
	sudo systemctl stop 'mtl-perf-*' 2>/dev/null || true
	sudo systemctl reset-failed 'mtl-perf-*' 2>/dev/null || true
	for group in /sys/fs/resctrl/mtl_perf_*; do
		[ ! -d "$group" ] || sudo rmdir "$group"
	done
	if [ -f /run/mtl-perf-compare.schemata ]; then
		sudo sh -c 'cat /run/mtl-perf-compare.schemata > /sys/fs/resctrl/schemata && rm /run/mtl-perf-compare.schemata'
	fi
}

# A codec choice reads "<word>: what it does". A jpegxs build also holds the plugin
# and its registry.
codec=${CODEC:-none}
codec=${codec%%:*}
parts=(dpdk mtl)
[[ $codec != jpegxs ]] || parts+=(jpegxs conf)

move_parts() {
	mkdir -p "$1"
	for part in "${parts[@]}"; do
		[ ! -e "${prefix}/${part}" ] || mv "${prefix}/${part}" "$1/"
	done
}

# Put back the workflow commit's parts that build moved aside.
restore_platform() {
	for part in "${parts[@]}"; do
		if [ -e "${slots}/platform/${part}" ]; then
			rm -rf "${prefix:?}/${part}"
			mv "${slots}/platform/${part}" "${prefix}/"
		fi
	done
}

# Commit $1 at $prefix, the path DPDK bakes in and each instance's slot is bound
# over. The build scripts are the workflow commit's, so old releases build alike.
build_slot() {
	local src="${slots}/src" pc libs part plugin i
	rm -rf "$src"
	for part in "${parts[@]}"; do
		rm -rf "${prefix:?}/${part}"
	done
	mkdir -p "$src"
	git -C "$root_dir" archive "$1" | tar -x -C "$src"
	cp "${root_dir}/script/build_dpdk.sh" "${root_dir}/script/common.sh" "${src}/script/"
	# versions.env up to v26.01 lacks DPDK_MTL_MINOR_VER, which it reads under set -u.
	DPDK_MTL_MINOR_VER='' MTL_INSTALL_PREFIX="${prefix}/mtl" bash "${src}/script/build_dpdk.sh" -f
	pc="${prefix}/dpdk/lib/x86_64-linux-gnu/pkgconfig:/usr/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig"
	PKG_CONFIG_PATH="$pc" meson setup "${src}/build" "$src" --prefix="${prefix}/mtl" -Dbuildtype=release
	ninja -C "${src}/build" install
	pc="${prefix}/mtl/lib/x86_64-linux-gnu/pkgconfig:${pc}"
	# Ahead of the linker cache, which names the .local_install the framework last ran.
	libs="${prefix}/mtl/lib/x86_64-linux-gnu:${prefix}/dpdk/lib/x86_64-linux-gnu"
	LD_LIBRARY_PATH="$libs" PKG_CONFIG_PATH="$pc" meson setup "${src}/rxtx" \
		"${src}/tests/tools/RxTxApp" -Dbuildtype=release
	LD_LIBRARY_PATH="$libs" ninja -C "${src}/rxtx"
	# Older releases have no install rule for RxTxApp.
	install -D -m 755 "${src}/rxtx/RxTxApp" "${prefix}/mtl/bin/RxTxApp"
	if [[ $codec == jpegxs ]]; then
		# The workflow commit's SVT for both builds, so both run the same codec.
		cp "${root_dir}/script/build_jpegxs.sh" "${src}/script/"
		mkdir -p "${prefix}/conf"
		echo '{}' >"${prefix}/conf/kahawai.json"
		MTL_INSTALL_PREFIX="${prefix}/mtl" bash "${src}/script/build_jpegxs.sh" \
			-v "$(sed -n 's/^SVT_JPEG_XS_VER=//p' "${root_dir}/versions.env")" \
			-p "${prefix}/jpegxs" -s "${slots}/svt" --config "${prefix}/conf/kahawai.json"
		# The plugin codes 8 sessions a registration, and MTL registers a path once:
		# 7 more links give 64, of which MTL's one st22 TX scheduler takes 60.
		plugin=$(jq -r '.plugins[0].path' "${prefix}/conf/kahawai.json")
		for i in 1 2 3 4 5 6 7; do
			ln -s "$(basename "$plugin")" "${plugin%.so}_${i}.so"
		done
		jq --arg p "${plugin%.so}" '.plugins += [range(1; 8) as $i |
			{enabled: 1, name: "st22_svt_jpegxs_\($i)", path: "\($p)_\($i).so"}]' \
			"${prefix}/conf/kahawai.json" >"${src}/kahawai.json"
		mv "${src}/kahawai.json" "${prefix}/conf/kahawai.json"
	fi
	rm -rf "$src"
}

# A name, a SHA or a ~ or ^ suffix: no option, and no newline for GITHUB_OUTPUT.
resolve() {
	[[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._/~^-]*$ ]] || return 1
	git rev-parse --verify --quiet "origin/$1^{commit}" ||
		git rev-parse --verify "$1^{commit}"
}

problems=()
refuse() { problems+=("$1"); }
report() {
	((${#problems[@]})) || return 0
	printf '::error::%s\n' "${problems[@]}" >&2
	{
		echo "perf-compare refused the inputs:"
		printf -- '- %s\n' "${problems[@]}"
	} >>"${GITHUB_STEP_SUMMARY:?GITHUB_STEP_SUMMARY is required}"
	exit 1
}

case "${1:-}" in
refs)
	# Every input that cannot run, at once and before the build. The numbers are
	# string inputs, checked first: the checks after them compute with them.
	for v in SESSIONS TEST_TIME ROUNDS; do
		[[ -z ${!v:-} || ${!v} =~ ^[1-9][0-9]*$ ]] || refuse "${v,,} must be a positive number: ${!v@Q}"
	done
	report
	# Each window needs two stats dumps, 10 s apart.
	((${TEST_TIME:-30} >= 30)) || refuse "test_time must be at least 30: ${TEST_TIME}"
	# What a 100G port carries at pc.stream_bps(): 20-bit pixels over 1125 of 1080
	# lines, 1406 bytes for 1320 of pixels.
	video=${VIDEO:-1080p59} sessions=${SESSIONS:-36} nics=${NICS:-single}
	most=$(awk -v h="${video%p*}" -v f="${video#*p}" 'BEGIN {
		fps[25] = 25; fps[29] = 30000 / 1001; fps[50] = 50; fps[59] = 60000 / 1001
		print int(100e9 / (h * h * 16 / 9 * 20 * fps[f] * 1125 / 1080 * 1406 / 1320))
	}')
	# As test_perf_compare.py refuses them.
	if [[ $codec == jpegxs ]]; then
		((sessions <= 60)) || refuse "codec jpegxs takes up to 60 sessions, as MTL puts a TX instance's st22 sessions on one scheduler, which takes 60: ${sessions}"
		[[ ${video%p*} == 1080 ]] || refuse "codec jpegxs takes 1080p: no other resolution has a planar clip short enough for each TX session's hugepages"
		[[ ${DMA:-none} == none ]] || refuse "codec jpegxs takes no dma: MTL's st22 RX copies with none"
		[[ ${PIXEL_FORMAT:-none} == none* || ${PIXEL_FORMAT} == yuv422p10le* ]] ||
			refuse "codec jpegxs takes pixel_format none or yuv422p10le: its encoder takes the planar clip as it is, and st22p converts nothing"
	elif [[ ${nics%%:*} == whole ]]; then
		((sessions <= 2 * most)) || refuse "${sessions} x ${video} take more than two 100G ports carry, $((2 * most))"
	elif ((sessions > most)); then
		refuse "${sessions} x ${video} take more than a 100G port carries, ${most}; nics whole puts a build on two, up to $((2 * most))"
	fi
	if [[ ${SINGLE_CORE_RX:-} == true || ${SINGLE_CORE_TX:-} == true ]] && ((sessions > 60)); then
		refuse "single core takes up to 60 sessions, as many as an MTL scheduler: ${sessions}"
	fi
	if [[ ${COMPLIANCE:-} == true && ${video%p*} == 4320 ]]; then
		refuse "compliance: EBU LIST cannot judge ${video}"
	fi
	if [[ ${GITHUB_EVENT_NAME:-} == schedule ]]; then
		# The main of the last nightly that passed against this main: GitHub starts a
		# night hours late, so a fixed window would leave commits to no night.
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
		name_a=$REF_A name_b=$REF_B
		sha_a=$(resolve "${REF_A:?REF_A is required}") || refuse "ref_a is no branch, tag or SHA here: ${REF_A@Q}"
		sha_b=$(resolve "${REF_B:?REF_B is required}") || refuse "ref_b is no branch, tag or SHA here: ${REF_B@Q}"
	fi
	# The SVT-JPEG-XS plugin of versions.env asks meson for MTL 26.09 or later.
	if [[ $codec == jpegxs ]]; then
		for sha in $(printf '%s\n' "$sha_a" "$sha_b" | sort -u); do
			version=$(git show "${sha}:VERSION" 2>/dev/null) || version=0.0
			IFS=. read -r major minor _ <<<"$version"
			((10#$major * 100 + 10#$minor >= 2609)) ||
				refuse "codec jpegxs takes builds of MTL 26.09 or later, as its SVT-JPEG-XS plugin asks: ${sha:0:9} is ${version}"
		done
	fi
	report
	printf '%s\n' "sha_a=${sha_a}" "sha_b=${sha_b}" "label_a=${name_a} ${sha_a:0:9}" \
		"label_b=${name_b} ${sha_b:0:9}" | tee -a "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
	;;
build)
	sudo rm -rf "$slots"
	# The workflow commit's DPDK and MTL serve the framework and step aside while
	# the slots build at their path; the host's ffmpeg links keep the rest.
	trap restore_platform EXIT
	move_parts "${slots}/platform"
	build_slot "${SHA_A:?SHA_A is required}"
	move_parts "${slots}/a"
	if [ "${SHA_B:?SHA_B is required}" = "$SHA_A" ]; then
		cp -a "${slots}/a" "${slots}/b"
	else
		build_slot "$SHA_B"
		move_parts "${slots}/b"
	fi
	du -sh "${slots}/a" "${slots}/b"
	;;
sync)
	{ command -v bpftrace && companion "command -v bpftrace"; } >/dev/null || {
		echo "::error::This host or the companion has no bpftrace, or the companion is unreachable" >&2
		exit 1
	}
	# Also what a killed run left on the companion.
	companion "$(declare -f sweep); sweep; sudo rm -rf '${slots}' && mkdir -p '${slots}'"
	for slot in a b; do
		rsync -az --delete -e "ssh ${ssh_options[*]}" "${slots}/${slot}/" \
			"${SHADOW_HOST_USER}@${SHADOW_HOST_IP}:${slots}/${slot}/"
	done
	echo "PERF_COMPARE_SLOTS=${slots}/a,${slots}/b" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
sweep) sweep ;;
clean)
	# The companion first, so a sweep that fails here does not skip it. Without the
	# credentials the job failed before it reached the companion.
	if [[ -n ${SHADOW_HOST_IP:-} && -n ${SHADOW_HOST_USER:-} ]]; then
		companion "$(declare -f sweep); sweep; sudo rm -rf '${slots}'" ||
			echo "::warning::Cleaning the companion failed" >&2
	fi
	sweep
	sudo rm -rf "$slots"
	;;
*)
	echo "Usage: $0 {refs|build|sync|sweep|clean}" >&2
	exit 2
	;;
esac
