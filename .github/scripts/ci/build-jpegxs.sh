#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
ci_dir="${root_dir}/.github/scripts/ci"
# shellcheck disable=SC1091
. "${root_dir}/versions.env"

bundle=${JPEGXS_ROOT:-"${root_dir}/.local_install/jpegxs"}
compiler_sha256=$(bash "${ci_dir}/compiler-identity.sh" producer)
[ "$(bash "${ci_dir}/compiler-identity.sh")" = "$compiler_sha256" ] || {
	echo "JPEG XS producer compiler does not match the CI toolchain contract" >&2
	exit 1
}
src="${root_dir}/.github/scripts/SVT-JPEG-XS-${SVT_JPEG_XS_VER}"
if [ ! -f "${src}/CMakeLists.txt" ] || [ "$(cat "${src}/.mtl-revision" 2>/dev/null)" != "$SVT_JPEG_XS_VER" ]; then
	rm -rf "$src"
	mkdir -p "$src"
	curl --fail --location --retry 3 --connect-timeout 15 --max-time 180 \
		"https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/${SVT_JPEG_XS_VER}.tar.gz" --output "${src}.tar.gz"
	tar -xzf "${src}.tar.gz" -C "$src" --strip-components=1
	rm -f "${src}.tar.gz"
	echo "$SVT_JPEG_XS_VER" >"${src}/.mtl-revision"
fi

if JPEGXS_ROOT="$bundle" bash "${ci_dir}/validate-jpegxs.sh" >/dev/null 2>&1; then
	echo "JPEG XS bundle is already valid"
	exit 0
fi

stage="${bundle}.tmp.$$"
trap 'rm -rf "$stage"' EXIT
rm -rf "${src}/Build/ci" "${src}/imtl-plugin/build-ci"
cmake -S "$src" -B "${src}/Build/ci" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$stage" -DBUILD_SHARED_LIBS=ON
cmake --build "${src}/Build/ci" --parallel "$(nproc)"
cmake --install "${src}/Build/ci"

jpeg_pc=$(find "$stage" -name SvtJpegxs.pc -type f -print -quit)
mtl_pc=$(find "${root_dir}/.local_install/mtl" -name mtl.pc -print -quit)
dpdk_pc=$(find "${root_dir}/.local_install/dpdk" -name libdpdk.pc -print -quit)
test -n "$jpeg_pc" -a -n "$mtl_pc" -a -n "$dpdk_pc"
export PKG_CONFIG_PATH="${jpeg_pc%/*}:${mtl_pc%/*}:${dpdk_pc%/*}"
meson setup "${src}/imtl-plugin/build-ci" "${src}/imtl-plugin" --buildtype release --prefix "$stage"
meson compile -C "${src}/imtl-plugin/build-ci"
meson install -C "${src}/imtl-plugin/build-ci"
sed -i "s|^prefix=.*|prefix=\${pcfiledir}/$(realpath --relative-to="${jpeg_pc%/*}" "$stage")|" "$jpeg_pc"

cat >"${stage}/bundle.env" <<EOF
schema=1
svt_jpeg_xs_revision=${SVT_JPEG_XS_VER}
architecture=$(uname -m)
compiler_sha256=${compiler_sha256}
source_hash=$(bash "${root_dir}/script/hash_sources.sh" -o /dev/stdout | sed -n 's/^jpegxs=//p')
EOF
(
	cd "$stage"
	find . -type l -printf '%p=%l\n' | LC_ALL=C sort >symlinks.manifest
	find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum >"${stage}.manifest"
)
mv "${stage}.manifest" "${stage}/manifest.sha256"

JPEGXS_ROOT="$stage" bash "${ci_dir}/validate-jpegxs.sh"
rm -rf "$bundle"
mv "$stage" "$bundle"
echo "JPEG XS bundle built at ${bundle}"
