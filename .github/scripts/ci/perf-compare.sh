#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Host side of perf-compare.yml (doc/perf_compare.md):
#   stash a|b  move the restored .local_install to its slot; b stays in place too,
#              for the framework's own use (nicctl.sh, MtlManager)
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
slots=${PERF_COMPARE_DIR:-$(dirname "$root_dir")/perf-compare}
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

case "${1:-}" in
stash)
	slot=${2:?usage: perf-compare.sh stash a|b}
	[ "$slot" != a ] || sudo rm -rf "$slots"
	mkdir -p "$slots"
	mv "${root_dir}/.local_install" "${slots}/${slot}"
	# Hard links: the same files, so the framework runs the B tree as it is.
	[ "$slot" != b ] || cp -al "${slots}/b" "${root_dir}/.local_install"
	du -sh "${slots}/${slot}"
	;;
sync)
	companion "sudo rm -rf '${slots}' && mkdir -p '${slots}' && command -v bpftrace >/dev/null" || {
		echo "::error::The companion has no bpftrace; it is host image content (doc/perf_compare.md)" >&2
		exit 1
	}
	command -v bpftrace >/dev/null || {
		echo "::error::This host has no bpftrace; it is host image content (doc/perf_compare.md)" >&2
		exit 1
	}
	rsync -az --delete -e "ssh ${ssh_options[*]}" "${slots}/" "${SHADOW_HOST_USER}@${SHADOW_HOST_IP}:${slots}/"
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
	echo "Usage: $0 {stash a|b|sync|sweep|clean}" >&2
	exit 2
	;;
esac
