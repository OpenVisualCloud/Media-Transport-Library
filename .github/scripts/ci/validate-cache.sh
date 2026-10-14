#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

ci_dir=$(dirname "${BASH_SOURCE[0]}")
component=$1
root=$(realpath "${ci_dir}/../../..")/.local_install/${component}

[ -d "$root" ] || {
	echo "${component} cache is missing: ${root}" >&2
	exit 1
}

while IFS= read -r -d '' link; do
	target=$(readlink -m "$link")
	[[ $target == "$root"/* ]] || {
		echo "${component} cache contains an external symlink: ${link} -> ${target}" >&2
		exit 1
	}
done < <(find "$root" -type l -print0)

case "$component" in
dpdk) find "$root" -name libdpdk.pc -print -quit | grep -q . ;;
mtl) find "$root" -name mtl.pc -print -quit | grep -q . ;;
ffmpeg) find "$root" -name libavcodec.pc -print -quit | grep -q . ;;
gstreamer | plugins) find "$root" -name '*.so' -print -quit | grep -q . ;;
jpegxs | ice) bash "${ci_dir}/validate-${component}.sh" ;;
*)
	echo "unknown cache component: ${component}" >&2
	exit 2
	;;
esac

echo "${component} cache: valid"
