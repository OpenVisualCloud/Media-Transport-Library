#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
release=$(uname -r)
bundle=${1:-${root_dir}/.local_install/ice}
ko="${bundle}/${release}/$(uname -m)/ice.ko"

die() {
	echo "$*" >&2
	exit 1
}

test -s "$ko" || die "no ice.ko for kernel ${release} in ${bundle}, which holds: $(find "$bundle" -mindepth 1 -maxdepth 1 -printf '%f ' 2>/dev/null)"
vermagic=$(modinfo -F vermagic "$ko")
[[ "${vermagic} " == "${release} "* ]] || die "ice.ko is built for '${vermagic}', not ${release}"
symbols=$(nm "$ko") || die "nm cannot read ${ko}"
# A compiler can keep the function only as a suffixed copy, such as .isra.0.
grep -Eq '[[:space:]]ice_vc_cfg_q_bw(\.|$)' <<<"$symbols" ||
	die "ice.ko is not the out-of-tree build: ice_vc_cfg_q_bw is missing"
want=$(awk -F= '/^ICE_VER=/ {print $2; exit}' "${root_dir}/versions.env")
version=$(modinfo -F version "$ko")
bash -c '. "$1/script/common.sh" && ice_driver_version_ok "$2" "$3"' _ "$root_dir" "$version" "$want" ||
	die "ice.ko is version '${version}', not the build of ICE ${want}"
echo "ICE bundle: valid (${release})"
