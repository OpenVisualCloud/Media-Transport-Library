#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Loads the cached ICE and IAVF modules -- they ship as one package -- and does
# nothing at all when the host already runs them.
#
# "Already runs them" is asked through srcversion: the .ko carries one, the
# kernel exports the srcversion of what it loaded, and the two match only for the
# same build. A loaded module cannot be replaced in place, so a mismatch means
# rmmod, and rmmod returns EBUSY while a VF exists or irdma is bound. That is
# what the teardown below is.

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

if loaded_is_cached; then
	echo "ICE/IAVF are already the cached modules, leaving them in place"
	exit 0
fi

# VFs of the ice PFs only. A card on another driver is none of our business.
for vf_count in /sys/class/net/*/device/sriov_numvfs; do
	[ -e "$vf_count" ] || continue
	device=$(dirname "$(readlink -f "$vf_count")")
	[ "$(basename "$(readlink -f "${device}/driver" 2>/dev/null)")" = ice ] || continue
	echo 0 >"$vf_count"
done

modprobe -r irdma || true
modprobe -r ice
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
