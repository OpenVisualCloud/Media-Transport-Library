#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"

bundle=${JPEGXS_ROOT:-"${root_dir}/.local_install/jpegxs"}
source_root=${SVT_JPEG_XS_SOURCE_ROOT:-"${root_dir}/.github/scripts"}
source_dir=$(bash "${root_dir}/.github/scripts/ci/jpegxs-source.sh" path "$source_root" "$SVT_JPEG_XS_VER")
archive="${source_dir}.tar.gz"
stage="${bundle}.tmp.$$"

mkdir -p "$(dirname "$source_dir")"

if ! bash "${root_dir}/.github/scripts/ci/jpegxs-source.sh" validate "$source_dir" "$SVT_JPEG_XS_VER"; then
	rm -rf "$source_dir" "$archive"
	curl --fail --location --retry 3 --connect-timeout 15 --max-time 180 \
		"https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/${SVT_JPEG_XS_VER}.tar.gz" \
		--output "$archive"
	mkdir -p "$source_dir"
	tar -xzf "$archive" -C "$source_dir" --strip-components=1
	rm -f "$archive"
	printf '%s\n' "$SVT_JPEG_XS_VER" >"${source_dir}/.mtl-revision"
fi

if JPEGXS_ROOT="$bundle" bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh" >/dev/null 2>&1; then
	echo "JPEG XS bundle is already valid"
	exit 0
fi

rm -rf "$stage"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage"

cmake -S "$source_dir" -B "${source_dir}/Build/ci" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$stage" \
	-DBUILD_SHARED_LIBS=ON
cmake --build "${source_dir}/Build/ci" --parallel "$(nproc)"
cmake --install "${source_dir}/Build/ci"

mtl_pc=$(find "${root_dir}/.local_install/mtl" -name mtl.pc -print -quit)
dpdk_pc=$(find "${root_dir}/.local_install/dpdk" -name libdpdk.pc -print -quit)
test -n "$mtl_pc" -a -n "$dpdk_pc"
jpeg_pc=$(find "$stage" -name SvtJpegxs.pc -print -quit)
test -n "$jpeg_pc"

rm -rf "${source_dir}/imtl-plugin/build-ci"
PKG_CONFIG_PATH="$(dirname "$jpeg_pc"):$(dirname "$mtl_pc"):$(dirname "$dpdk_pc")" \
	meson setup "${source_dir}/imtl-plugin/build-ci" "${source_dir}/imtl-plugin" \
		--buildtype release --prefix "$stage"
PKG_CONFIG_PATH="$(dirname "$jpeg_pc"):$(dirname "$mtl_pc"):$(dirname "$dpdk_pc")" \
	meson compile -C "${source_dir}/imtl-plugin/build-ci"
meson install -C "${source_dir}/imtl-plugin/build-ci"

while IFS= read -r pc_file; do
	relative_prefix=$(realpath --relative-to="$(dirname "$pc_file")" "$stage")
	sed -i "s|^prefix=.*|prefix=\${pcfiledir}/${relative_prefix}|" "$pc_file"
done < <(find "$stage" -name SvtJpegxs.pc -type f)

cat >"${stage}/bundle.env" <<EOF
schema=1
svt_jpeg_xs_revision=${SVT_JPEG_XS_VER}
architecture=$(uname -m)
compiler=$(cc -dumpfullversion -dumpversion)
EOF
(cd "$stage" && find . -type l -printf '%p=%l\n' | LC_ALL=C sort >symlinks.manifest)
manifest="${stage}.manifest"
(cd "$stage" && find . -type f ! -name manifest.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum >"$manifest")
mv "$manifest" "$stage/manifest.sha256"

JPEGXS_ROOT="$stage" bash "${root_dir}/.github/scripts/ci/validate-jpegxs.sh"
rm -rf "$bundle"
mv "$stage" "$bundle"
trap - EXIT
echo "JPEG XS bundle built at ${bundle}"