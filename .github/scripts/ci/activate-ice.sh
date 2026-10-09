#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
kernel_release=$(uname -r)
ko="${root_dir}/.local_install/ice/${kernel_release}/$(uname -m)/ice.ko"
pfs=/sys/bus/pci/drivers/ice

bash "${root_dir}/.github/scripts/ci/validate-ice.sh"
hash ethtool

install -D -m 0644 "$ko" "/lib/modules/${kernel_release}/updates/drivers/net/ethernet/intel/ice/ice.ko"
depmod -a "$kernel_release"

# Always reload: a matching srcversion does not clear VFs, irdma or state left by an earlier job.
for numvfs in "$pfs"/0000:*/sriov_numvfs; do
	[ ! -e "$numvfs" ] || echo 0 >"$numvfs"
done
modprobe -r irdma || true
[ ! -d /sys/module/ice ] || modprobe -r ice
modprobe ice

[ "$(cat /sys/module/ice/srcversion)" = "$(modinfo -F srcversion "$ko")" ] || {
	echo "ICE did not come back up as the cached module" >&2
	exit 1
}

missing=""
for pf in "$pfs"/0000:*; do
	[ ! -e "$pf" ] || [ -d "${pf}/net" ] || missing="${missing} $(basename "$pf")"
done
if [ -n "$missing" ]; then
	echo "ICE loaded, but these PFs registered no netdev:${missing}; re-run, or see dmesg" >&2
	exit 1
fi

has_hw_timestamp() {
	local netdev caps
	for netdev in "$1"/net/*; do
		caps=$(LC_ALL=C ethtool -T "$(basename "$netdev")") || return 1
		grep -Eq '^[[:space:]]*hardware-receive[[:space:]]*$' <<<"$caps" || return 1
		grep -Eq '^PTP Hardware Clock: [0-9]+[[:space:]]*$' <<<"$caps" || return 1
	done
}

# A PF probed before its shared-clock owner has no PHC until it is rebound; only reported, as few jobs capture.
for pf in "$pfs"/0000:*; do
	[ -e "$pf" ] || continue
	has_hw_timestamp "$pf" && continue
	bdf=$(basename "$pf")
	numvfs=$(cat "${pf}/sriov_numvfs" 2>/dev/null) || numvfs=unreadable
	if [ "$numvfs" != 0 ]; then
		echo "ICE ${bdf}: skipping PTP recovery, sriov_numvfs is ${numvfs}" >&2
		continue
	fi
	echo "ICE ${bdf}: retrying probe for hardware RX timestamps and PHC"
	for action in unbind bind; do
		timeout --kill-after=5s 30s tee "${pfs}/${action}" <<<"$bdf" >/dev/null || {
			echo "ICE ${bdf}: ${action} failed or timed out during PTP recovery" >&2
			exit 1
		}
	done
	has_hw_timestamp "$pf" ||
		echo "ICE ${bdf}: still no hardware RX timestamps or PHC after rebind" >&2
done

echo "ICE loaded from the cached module"
