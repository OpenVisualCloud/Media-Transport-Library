#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Loads the cached ICE module, or leaves the running driver alone when it is
# already that module.
#
# Neither half is one command. Nothing under /sys/module/ice is a hash of the
# loaded module, so "already that module" has to be asked indirectly: the cached
# artifact is the file at the updates/ path modprobe prefers, and the loaded
# driver reports the same srcversion as that file plus the Kahawai version. And
# a loaded module cannot be replaced in place -- rmmod is the only way in, and it
# returns EBUSY while VFs exist, irdma is bound or MtlManager holds a port, which
# is all the teardown below is.
#
# With --check it only answers the question -- exit 0 when the running driver is
# the cached module, 1 when it is not -- and changes nothing, so a caller that
# has no business replacing a module can still tell whether one needs replacing.

set -euo pipefail

check_only=0
if [ "${1:-}" = "--check" ]; then
	check_only=1
	shift
fi

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"

kernel_release=$(uname -r)
artifact="${root_dir}/.local_install/ice/${kernel_release}/$(uname -m)/ice.ko"
installed="/lib/modules/${kernel_release}/updates/drivers/net/ethernet/intel/ice/ice.ko"

bash "${root_dir}/.github/scripts/ci/validate-ice.sh"
desired=$(sha256sum "$artifact" | cut -d' ' -f1)

is_current() {
	[ -f "$installed" ] &&
		[ "$(sha256sum "$installed" | cut -d' ' -f1)" = "$desired" ] &&
		[ "$(cat /sys/module/ice/srcversion 2>/dev/null)" = "$(modinfo -F srcversion "$installed")" ] &&
		[ "$(cat /sys/module/ice/version 2>/dev/null)" = "Kahawai_${ICE_VER}" ]
}

if is_current; then
	echo "ICE is already the cached module ${desired}, leaving it loaded"
	exit 0
fi

[ "$check_only" -eq 0 ] || {
	echo "the running ice driver is not the cached module ${desired}" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || {
	echo "loading ICE needs root: sudo task ci:activate-ice" >&2
	exit 1
}

# SIGTERM is asynchronous, so give each user a few seconds to go: rmmod hitting
# EBUSY reports the module, never who was holding it.
for process in MtlManager RxTxApp; do
	pkill -TERM -x "$process" || continue
	for _ in 1 2 3 4 5; do
		pgrep -x "$process" >/dev/null || break
		sleep 1
	done
done

# VFs of the ice PFs only. A card on another driver is none of our business.
for vf_count in /sys/class/net/*/device/sriov_numvfs; do
	[ -e "$vf_count" ] || continue
	device=$(dirname "$(readlink -f "$vf_count")")
	[ "$(basename "$(readlink -f "${device}/driver" 2>/dev/null)")" = ice ] || continue
	echo 0 >"$vf_count"
done

if [ -d /sys/module/irdma ]; then
	reload_irdma=1
else
	reload_irdma=0
fi
modprobe -r irdma || true
modprobe -r ice

install -D -m 0644 "$artifact" "$installed"
depmod -a "$kernel_release"
modprobe ice
is_current || {
	echo "ICE did not come back up as the cached module ${desired}" >&2
	exit 1
}

# Put back what was unloaded to get here. Nothing in the suite uses RDMA, so a
# host that will not reload it is worth a line and no more.
[ "$reload_irdma" -eq 0 ] || modprobe irdma ||
	echo "warning: irdma did not reload, it stays unloaded" >&2
echo "ICE loaded from the cached module ${desired}"

# VFs are not recreated here: every consumer builds the VF state it needs, and
# does it idempotently -- a gtest job runs bind-test-ports.sh, and the acceptance
# harness has Nicctl.create_vfs().
