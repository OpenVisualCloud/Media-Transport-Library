#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

die() {
	echo "JPEG XS $*" >&2
	exit 1
}
has() {
	[ -n "$(find . -name "$1" -type "$2" -print -quit)" ] || die "bundle lacks $1 (type $2)"
}

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"
cd "${JPEGXS_ROOT:-${root_dir}/.local_install/jpegxs}"

sha256sum --quiet -c manifest.sha256 || die "files do not match manifest.sha256"
find . -type l -printf '%p=%l\n' | LC_ALL=C sort | cmp -s - symlinks.manifest || die "symlinks do not match symlinks.manifest"
has 'libSvtJpegxs.so*' f
has 'libSvtJpegxs.so*' l
has libst_plugin_st22_svt_jpeg_xs.so f
pc_file=$(find . -name SvtJpegxs.pc -type f -print -quit)
[ -n "$pc_file" ] || die "bundle lacks SvtJpegxs.pc"
# shellcheck disable=SC2016
grep -Fq '${pcfiledir}' "$pc_file" || die "pkg-config prefix is not relocatable"
PKG_CONFIG_PATH=$(dirname "$pc_file") pkg-config --exists SvtJpegxs || die "pkg-config cannot resolve SvtJpegxs"

source_hash=${JPEGXS_EXPECTED_SOURCE_HASH:-$(bash "${root_dir}/script/hash_sources.sh" -o /dev/stdout | sed -n 's/^jpegxs=//p')}
# An empty hash would match an empty source_hash line and prove nothing.
[ -n "$source_hash" ] || die "source hash of this checkout is empty"
compiler_sha256=${JPEGXS_EXPECTED_COMPILER_SHA256:-$(bash "${root_dir}/.github/scripts/ci/compiler-identity.sh" producer)}
for line in schema=1 "architecture=$(uname -m)" "svt_jpeg_xs_revision=${SVT_JPEG_XS_VER}" \
	"source_hash=${source_hash}" "compiler_sha256=${compiler_sha256}"; do
	grep -qxF "$line" bundle.env || die "bundle.env lacks ${line}"
done

echo "JPEG XS bundle: valid"
