#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
component=${1:?usage: validate-cache.sh COMPONENT}
install_root=${LOCAL_INSTALL_ROOT:-"${root_dir}/.local_install"}
component_root="${install_root}/${component}"

test -d "$component_root" || {
	echo "${component} cache is missing: ${component_root}" >&2
	exit 1
}

while IFS= read -r -d '' link; do
	target=$(readlink -m "$link")
	case "$target" in
	"${component_root}"/*) ;;
	*)
		echo "${component} cache contains an external symlink: ${link} -> ${target}" >&2
		exit 1
		;;
	esac
done < <(find "$component_root" -type l -print0)

case "$component" in
dpdk) find "$component_root" -name libdpdk.pc -print -quit | grep -q . ;;
mtl) find "$component_root" -name mtl.pc -print -quit | grep -q . ;;
ffmpeg) find "$component_root" -name libavcodec.pc -print -quit | grep -q . ;;
gstreamer | plugins) find "$component_root" -name '*.so' -print -quit | grep -q . ;;
jpegxs) JPEGXS_ROOT="$component_root" bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh" ;;
ice) ICE_BUNDLE_ROOT="$component_root" bash "${root_dir}/.github/scripts/ci/validate-ice.sh" ;;
*)
	echo "unknown cache component: ${component}" >&2
	exit 2
	;;
esac

echo "${component} cache: valid"
