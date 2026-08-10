#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
bundle=${JPEGXS_ROOT:-"${root_dir}/.local_install/jpegxs"}
manifest="${bundle}/manifest.sha256"

test -s "${bundle}/bundle.env" || {
	echo "JPEG XS bundle metadata is missing" >&2
	exit 1
}
test -s "$manifest" || {
	echo "JPEG XS bundle manifest is missing" >&2
	exit 1
}
test -f "${bundle}/symlinks.manifest" || {
	echo "JPEG XS symlink manifest is missing" >&2
	exit 1
}

(cd "$bundle" && sha256sum --quiet -c manifest.sha256)
(cd "$bundle" && find . -type l -printf '%p=%l\n' | LC_ALL=C sort | cmp -s - symlinks.manifest) || {
	echo "JPEG XS symlink topology does not match its manifest" >&2
	exit 1
}
find "$bundle" -name 'libSvtJpegxs.so*' -type f -print -quit | grep -q . || {
	echo "JPEG XS runtime library is missing" >&2
	exit 1
}
find "$bundle" -name 'libSvtJpegxs.so*' -type l -print -quit | grep -q . || {
	echo "JPEG XS runtime symlinks are missing" >&2
	exit 1
}
pc_file=$(find "$bundle" -name SvtJpegxs.pc -type f -print -quit)
test -n "$pc_file" || {
	echo "JPEG XS pkg-config metadata is missing" >&2
	exit 1
}
# shellcheck disable=SC2016
grep -Fq '${pcfiledir}' "$pc_file" || {
	echo "JPEG XS pkg-config prefix is not relocatable" >&2
	exit 1
}
grep -q "^architecture=$(uname -m)$" "${bundle}/bundle.env" || {
	echo "JPEG XS architecture does not match this host" >&2
	exit 1
}
expected_compiler_sha256=${JPEGXS_EXPECTED_COMPILER_SHA256:-$(bash "${root_dir}/.github/scripts/ci/compiler-identity.sh")}
grep -q "^compiler_sha256=${expected_compiler_sha256}$" "${bundle}/bundle.env" || {
	echo "JPEG XS compiler identity does not match this cache key" >&2
	exit 1
}
find "$bundle" -name libst_plugin_st22_svt_jpeg_xs.so -type f -print -quit | grep -q . || {
	echo "JPEG XS MTL bridge plugin is missing" >&2
	exit 1
}

pkg_dir=$(dirname "$pc_file")
PKG_CONFIG_PATH="$pkg_dir" pkg-config --exists SvtJpegxs || {
	echo "JPEG XS pkg-config entry cannot be resolved" >&2
	exit 1
}

echo "JPEG XS bundle: valid"