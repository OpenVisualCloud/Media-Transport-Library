#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

ci_dir=$(dirname "${BASH_SOURCE[0]}")
component=$1
# LOCAL_INSTALL_ROOT: validate a tree somewhere else, as the local CI runner does.
root=${LOCAL_INSTALL_ROOT:-$(realpath "${ci_dir}/../../../..")/.local_install}/${component}

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
mtl)
	find "$root" -name mtl.pc -print -quit | grep -q .
	[ -f "${root%/*}/conf/kahawai.json" ] || {
		echo "mtl cache lacks the plugin registry ${root%/*}/conf/kahawai.json" >&2
		exit 1
	}
	;;
conf)
	# Every plugin the registry enables is installed at its path.
	python3 - "${root}/kahawai.json" <<'PY'
import json
import os
import sys

missing = [item["path"] for item in json.load(open(sys.argv[1]))["plugins"]
           if item.get("enabled") and not os.path.isfile(item["path"])]
for path in missing:
    print(f"{sys.argv[1]} enables a plugin that is not installed: {path}", file=sys.stderr)
sys.exit(1 if missing else 0)
PY
	;;
ffmpeg) find "$root" -name libavcodec.pc -print -quit | grep -q . ;;
gstreamer | plugins) find "$root" -name '*.so' -print -quit | grep -q . ;;
jpegxs)
	pc=$(find "$root" -name SvtJpegxs.pc -type f -print -quit)
	[ -n "$pc" ] || {
		echo "jpegxs cache lacks SvtJpegxs.pc" >&2
		exit 1
	}
	# shellcheck disable=SC2016
	grep -Fq '${pcfiledir}' "$pc" || {
		echo "jpegxs cache: SvtJpegxs.pc is not relocatable" >&2
		exit 1
	}
	find "$root" -name libst_plugin_st22_svt_jpeg_xs.so -type f -print -quit | grep -q .
	;;
ice) bash "${ci_dir}/validate-ice.sh" ;;
*)
	echo "unknown cache component: ${component}" >&2
	exit 2
	;;
esac

echo "${component} cache: valid"
