#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Loads the cached ICE and IAVF modules -- they ship as one package. The load is
# unconditional: the modules that were loaded before are taken out, and the ones
# in the cache are put in their place.
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
# cached module, or the next auto-load brings back the old one. iavf is the one
# that needs this -- the kernel auto-loads it when a VF first appears, so its file
# matters even while nothing has it loaded.
install -D -m 0644 "${artifact_dir}/ice.ko" "${updates_dir}/ice/ice.ko"
install -D -m 0644 "${artifact_dir}/iavf.ko" "${updates_dir}/iavf/iavf.ko"
depmod -a "$kernel_release"

# Whether the modules the kernel holds now are the ones in the cache. Only the
# check that follows the load reads this: the load itself does not ask.
#
# ice is the PF driver and is always loaded on a host that needs it, so it must be
# loaded and match. iavf is the VF driver: with no VFs it may not be loaded at
# all, which is not a mismatch.
loaded_is_cached() {
	local module
	for module in ice iavf; do
		if [ ! -d "/sys/module/${module}" ]; then
			[ "$module" = iavf ] || return 1
			continue
		fi
		[ "$(cat "/sys/module/${module}/srcversion")" = \
			"$(modinfo -F srcversion "${artifact_dir}/${module}.ko")" ] || return 1
	done
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
# The VF teardown above cleared every VF on the ice PFs, so a loaded iavf now has
# no devices and can be replaced; an unloaded one is left to the auto-load.
if [ -d /sys/module/iavf ]; then
	modprobe -r iavf
	modprobe iavf
fi

loaded_is_cached || {
	echo "ICE/IAVF did not come back up as the cached modules" >&2
	exit 1
}
echo "ICE/IAVF loaded from the cached modules"

# VFs are not recreated here: every consumer builds the VF state it needs, and
# does it idempotently -- a gtest job runs bind-test-ports.sh, and the acceptance
# harness has Nicctl.create_vfs().
