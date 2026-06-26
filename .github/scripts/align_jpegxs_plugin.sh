#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

lib_so="libst_plugin_st22_svt_jpeg_xs.so"
need_build=0

# Check plugin .so
if ! ldconfig -p 2>/dev/null | grep -q "${lib_so}" &&
	! test -f /usr/local/lib/x86_64-linux-gnu/${lib_so} &&
	! test -f /usr/local/lib64/${lib_so}; then
	echo "MTL JPEG-XS plugin not found."
	need_build=1
fi

# Check core SvtJpegxs version
lib_path=$(ldconfig -p 2>/dev/null | grep -oE '/[^[:space:]]*libSvtJpegxs\.so\.0' | head -n1)
if [ -n "${lib_path}" ]; then
	real_lib=$(readlink -f "${lib_path}")
	if echo "${real_lib}" | grep -q '0.9.0'; then
		echo "Outdated core SvtJpegxs (${real_lib}) detected on host."
		need_build=1
	fi
else
	echo "Core SvtJpegxs library not found."
	need_build=1
fi

if [ "${need_build}" -eq 1 ]; then
	echo "::group::Automated build and sync of SVT-JPEG-XS core and plugin"
	sudo rm -rf /tmp/SVT-JPEG-XS
	git clone --depth 1 https://github.com/OpenVisualCloud/SVT-JPEG-XS.git /tmp/SVT-JPEG-XS

	echo "=== Building SvtJpegXS core ==="
	pushd /tmp/SVT-JPEG-XS/Build/linux
	./build.sh release
	sudo ./build.sh install
	popd

	echo "=== Deploying SvtJpegXS headers ==="
	sudo mkdir -p /usr/local/include/svt-jpegxs
	sudo cp -f /tmp/SVT-JPEG-XS/Source/API/*.h /usr/local/include/svt-jpegxs/

	echo "=== Building imtl-plugin bridge ==="
	pushd /tmp/SVT-JPEG-XS/imtl-plugin
	rm -rf build

	# Support .local_install custom prefix if present in the workspace
	if [ -n "${GITHUB_WORKSPACE:-}" ] && [ -d "${GITHUB_WORKSPACE}/.local_install/mtl" ]; then
		export PKG_CONFIG_PATH="${GITHUB_WORKSPACE}/.local_install/mtl/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}"
	fi

	meson setup build
	meson compile -C build
	sudo meson install -C build
	popd

	sudo ldconfig
	echo "::endgroup::"
	echo "SVT-JPEG-XS core and plugin successfully aligned/synchronized!"
else
	echo "SVT-JPEG-XS core and plugin are already up-to-date. Alignment skipped."
fi
