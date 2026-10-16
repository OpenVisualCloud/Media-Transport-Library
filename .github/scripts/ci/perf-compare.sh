#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Host side of perf-compare.yml (doc/perf_compare.md):
#   build      build SHA_A and SHA_B into the slots a and b
#   sync       copy both slots to the same path on the companion, check that both
#              hosts can trace, and name the slots in PERF_COMPARE_SLOTS
#   sweep      stop the units and undo the cache partitioning a run left behind,
#              on this host
#   clean      sweep both hosts and remove the slots

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# Next to the workspace, not in it: checkout-clean removes .local_install and the
# checkout cleans untracked files. Not in RUNNER_TEMP either: the companion is a
# runner as well, which empties its _temp whenever it starts a job.
slots=$(dirname "$root_dir")/perf-compare
prefix="${root_dir}/.local_install"
ssh_options=(-o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=30)

companion() {
	# shellcheck disable=SC2029
	ssh "${ssh_options[@]}" "${SHADOW_HOST_USER:?SHADOW_HOST_USER is required}@${SHADOW_HOST_IP:?SHADOW_HOST_IP is required}" "$@"
}

# The unit prefix, group prefix and save file of tests/acceptance/mtl_engine/perf_compare.py.
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

# DPDK, libmtl and RxTxApp of commit $1, at $prefix: DPDK bakes that path in, and the
# instance binds its slot over it. The build scripts are the workflow commit's, so a
# release that predates prefix installs builds alike; versions.env and the patches
# stay the ref's.
build_slot() {
	local src="${slots}/src" pc libs
	rm -rf "$src" "$prefix"
	mkdir -p "$src"
	git -C "$root_dir" archive "$1" | tar -x -C "$src"
	cp "${root_dir}/script/build_dpdk.sh" "${root_dir}/script/common.sh" "${src}/script/"
	MTL_INSTALL_PREFIX="${prefix}/dpdk" bash "${src}/script/build_dpdk.sh" -f
	pc="${prefix}/dpdk/lib/x86_64-linux-gnu/pkgconfig:/usr/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig"
	PKG_CONFIG_PATH="$pc" meson setup "${src}/build" "$src" --prefix="${prefix}/mtl" -Dbuildtype=release
	ninja -C "${src}/build" install
	pc="${prefix}/mtl/lib/x86_64-linux-gnu/pkgconfig:${pc}"
	# ld resolves the libraries libmtl needs through LD_LIBRARY_PATH before the host's
	# linker cache, which names whatever .local_install the framework last ran.
	libs="${prefix}/mtl/lib/x86_64-linux-gnu:${prefix}/dpdk/lib/x86_64-linux-gnu"
	LD_LIBRARY_PATH="$libs" PKG_CONFIG_PATH="$pc" meson setup "${src}/rxtx" \
		"${src}/tests/tools/RxTxApp" -Dbuildtype=release
	LD_LIBRARY_PATH="$libs" ninja -C "${src}/rxtx"
	# Copied, not installed: RxTxApp has no install rule in older releases.
	install -D -m 755 "${src}/rxtx/RxTxApp" "${prefix}/mtl/bin/RxTxApp"
	rm -rf "$src"
}

case "${1:-}" in
build)
	[[ ${SHA_A:-} =~ ^[0-9a-f]{40}$ && ${SHA_B:-} =~ ^[0-9a-f]{40}$ ]] || {
		echo "SHA_A and SHA_B must be full commit SHAs (git rev-parse <ref>)" >&2
		exit 2
	}
	sudo rm -rf "$slots"
	mkdir -p "$slots"
	# The workflow commit's own tree serves the framework (MtlManager, nicctl.sh);
	# it steps aside while the slots are built at its path.
	mv "$prefix" "${slots}/platform"
	trap 'rm -rf "$prefix"; mv "${slots}/platform" "$prefix"' EXIT
	build_slot "$SHA_A"
	mv "$prefix" "${slots}/a"
	if [ "$SHA_B" = "$SHA_A" ]; then
		cp -a "${slots}/a" "${slots}/b"
	else
		build_slot "$SHA_B"
		mv "$prefix" "${slots}/b"
	fi
	du -sh "${slots}/a" "${slots}/b"
	;;
sync)
	command -v bpftrace >/dev/null || {
		echo "::error::This host has no bpftrace; it is host image content (doc/ci_runner_setup.md)" >&2
		exit 1
	}
	rc=0
	companion "command -v bpftrace >/dev/null" || rc=$?
	if ((rc == 255)); then
		echo "::error::The companion is not reachable over ssh" >&2
		exit 1
	elif ((rc)); then
		echo "::error::The companion has no bpftrace; it is host image content (doc/ci_runner_setup.md)" >&2
		exit 1
	fi
	companion "sudo rm -rf '${slots}' && mkdir -p '${slots}'"
	for slot in a b; do
		rsync -az --delete -e "ssh ${ssh_options[*]}" "${slots}/${slot}/" \
			"${SHADOW_HOST_USER}@${SHADOW_HOST_IP}:${slots}/${slot}/"
	done
	echo "PERF_COMPARE_SLOTS=${slots}/a,${slots}/b" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
sweep) sweep ;;
clean)
	sweep
	# Without the credentials the job failed before it reached the companion.
	if [[ -n ${SHADOW_HOST_IP:-} && -n ${SHADOW_HOST_USER:-} ]]; then
		companion "$(declare -f sweep); sweep; sudo rm -rf '${slots}'" || true
	fi
	sudo rm -rf "$slots"
	;;
*)
	echo "Usage: $0 {build|sync|sweep|clean}" >&2
	exit 2
	;;
esac
