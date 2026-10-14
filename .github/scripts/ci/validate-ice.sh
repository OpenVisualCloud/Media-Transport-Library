#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
release=$(uname -r)
ko="${ICE_BUNDLE_ROOT:-${root_dir}/.local_install/ice}/${release}/$(uname -m)/ice.ko"

die() {
	echo "$*" >&2
	exit 1
}

test -s "$ko" || die "ice.ko is missing: ${ko}"
vermagic=$(modinfo -F vermagic "$ko")
[[ "${vermagic} " == "${release} "* ]] || die "ice.ko is built for '${vermagic}', not ${release}"
# Not grep -q: closing the pipe early makes pipefail report nm's SIGPIPE.
nm "$ko" | grep '[[:space:]]ice_vc_cfg_q_bw$' >/dev/null ||
	die "ice.ko is not the Kahawai build: ice_vc_cfg_q_bw is missing"
echo "ICE bundle: valid (${release})"
