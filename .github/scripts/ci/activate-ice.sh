#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Loads the cached ICE module. The load is unconditional: the module that was
# loaded before is taken out, and the one in the cache is put in its place.
#
# It was conditional on srcversion before, and that was not enough. A srcversion
# match says only that the loaded module was built from the same source. It does
# not say the driver is in a state the tests can use: a job that ran before can
# leave the driver holding its own state, a set of VFs, or irdma bound to it, and
# a module that the distribution supplies can carry the same source with other
# build options. A reload costs seconds, and it is the only way the suite starts
# from the module in the cache and from nothing else.
#
# A loaded module cannot be replaced in place, so the reload needs rmmod, and
# rmmod returns EBUSY while a VF exists or irdma is bound. That is what the
# teardown below is.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
kernel_release=$(uname -r)
artifact_dir="${root_dir}/.local_install/ice/${kernel_release}/$(uname -m)"
updates_dir="/lib/modules/${kernel_release}/updates/drivers/net/ethernet/intel"

[ "$(id -u)" -eq 0 ] || {
	echo "loading ICE needs root: sudo task ci:activate-ice" >&2
	exit 1
}

bash "${root_dir}/.github/scripts/ci/validate-ice.sh"

# Unconditional: the file at the updates/ path modprobe prefers has to be the
# cached module, or the next auto-load brings back the old one.
install -D -m 0644 "${artifact_dir}/ice.ko" "${updates_dir}/ice/ice.ko"
depmod -a "$kernel_release"

# Whether the module the kernel holds now is the one in the cache. Only the check
# that follows the load reads this: the load itself does not ask.
#
# ice is the PF driver and is always loaded on a host that needs it, so it must be
# loaded and match.
loaded_is_cached() {
	[ -d /sys/module/ice ] || return 1
	[ "$(cat /sys/module/ice/srcversion)" = \
		"$(modinfo -F srcversion "${artifact_dir}/ice.ko")" ]
}

# VFs of the ice PFs only. A card on another driver is none of our business.
for vf_count in /sys/class/net/*/device/sriov_numvfs; do
	[ -e "$vf_count" ] || continue
	device=$(dirname "$(readlink -f "$vf_count")")
	[ "$(basename "$(readlink -f "${device}/driver" 2>/dev/null)")" = ice ] || continue
	echo 0 >"$vf_count"
done

modprobe -r irdma || true
# Asked, because `modprobe -r` of a module that is not loaded fails, and the load
# runs on every job now -- a host that has no ice loaded yet is not an error.
if [ -d /sys/module/ice ]; then
	modprobe -r ice
fi
modprobe ice

loaded_is_cached || {
	echo "ICE did not come back up as the cached module" >&2
	exit 1
}

# ICE probes synchronously. A missing netdev after modprobe is a failed probe;
# reject it before VF configuration can block on the incomplete PF.
missing=""
for pf in /sys/bus/pci/drivers/ice/0000:*; do
	[ -e "$pf" ] || continue
	[ -d "${pf}/net" ] || missing="${missing} $(basename "$pf")"
done
if [ -n "$missing" ]; then
	echo "ICE loaded, but these PFs registered no netdev:${missing}" >&2
	echo "Re-running reloads the driver, which has recovered this before. If it" >&2
	echo "persists, dmesg carries the probe error." >&2
	exit 1
fi

command -v ethtool >/dev/null || {
	echo "ICE timestamp validation requires ethtool" >&2
	exit 1
}

pf_has_hw_timestamp() {
	local netdev capabilities
	for netdev in "$1"/net/*; do
		[ -e "$netdev" ] || return 1
		capabilities=$(LC_ALL=C ethtool -T "$(basename "$netdev")") || return 1
		grep -Eq '^[[:space:]]*hardware-receive[[:space:]]*$' <<<"$capabilities" || return 1
		grep -Eq '^PTP Hardware Clock: [0-9]+[[:space:]]*$' <<<"$capabilities" || return 1
	done
}

# A PF probed before its shared-clock owner comes up without a PHC and needs a
# retry once every probe has finished. Only the sniff PF's timestamps decide a
# compliance verdict, and the acceptance suite gates that one interface itself,
# so a PF still without a PHC here is reported rather than failed: every job
# that validates an ICE host runs this, and most of them never capture.
for pf in /sys/bus/pci/drivers/ice/0000:*; do
	[ -e "$pf" ] || continue
	pf_has_hw_timestamp "$pf" && continue
	bdf=$(basename "$pf")
	numvfs=$(cat "${pf}/sriov_numvfs" 2>/dev/null) || numvfs=unreadable
	if [ "$numvfs" != 0 ]; then
		echo "ICE ${bdf}: skipping PTP recovery, sriov_numvfs is ${numvfs}" >&2
		continue
	fi
	echo "ICE ${bdf}: retrying probe for hardware RX timestamps and PHC"
	for action in unbind bind; do
		# shellcheck disable=SC2016
		if ! timeout --kill-after=5s 30s bash -c 'printf "%s\n" "$1" > "$2"' _ \
			"$bdf" "/sys/bus/pci/drivers/ice/${action}"; then
			echo "ICE ${bdf}: ${action} failed or timed out during PTP recovery" >&2
			exit 1
		fi
	done
	pf_has_hw_timestamp "$pf" ||
		echo "ICE ${bdf}: still no hardware RX timestamps or PHC after rebind;" \
			"a capture on this PF would use software timestamps" >&2
done

echo "ICE loaded from the cached module"

# VFs are not recreated here: every consumer builds the VF state it needs, and
# does it idempotently -- a gtest job runs bind-test-ports.sh, and the acceptance
# harness has Nicctl.create_vfs().
