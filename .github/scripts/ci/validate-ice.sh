#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Is the restored ICE+IAVF bundle a pair of modules this host can load?
#
# The cache key answers most of that already: it carries the patch hash, the
# kernel release and the architecture, so a bundle that arrives here was built
# from this checkout for a host like this one. Two questions are left, and both
# are read straight off the files:
#
#   vermagic must match the running kernel, or modprobe rejects the module; and
#   ice.ko must carry ice_vc_cfg_q_bw, the Kahawai VF rate limiter MTL paces
#   with. A stock driver loads and carries traffic, then MTL dies in
#   iavf_tm_node_add a minute into the first test.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
kernel_release=$(uname -r)
bundle_root=${ICE_BUNDLE_ROOT:-"${root_dir}/.local_install/ice"}
artifact_dir="${bundle_root}/${kernel_release}/$(uname -m)"

for module in ice iavf; do
	path="${artifact_dir}/${module}.ko"
	test -s "$path" || {
		echo "${module}.ko is missing: ${path}" >&2
		exit 1
	}
	vermagic=$(modinfo -F vermagic "$path")
	case "$vermagic" in
	"${kernel_release}" | "${kernel_release} "*) ;;
	*)
		echo "${module}.ko is built for '${vermagic}', not ${kernel_release}" >&2
		exit 1
		;;
	esac
done

# No grep -q: it closes the pipe on the first match, and pipefail then reports
# nm's SIGPIPE as the status of a search that succeeded.
nm "${artifact_dir}/ice.ko" | grep '[[:space:]]ice_vc_cfg_q_bw$' >/dev/null || {
	echo "ice.ko is not the Kahawai build: ice_vc_cfg_q_bw is missing" >&2
	exit 1
}

echo "ICE+IAVF bundle: valid (${kernel_release})"
