#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation

set -e

enable_gpu=false
enable_jpegxs=false
script_path="$(dirname "$(readlink -f "$0")")"
VERSIONS_ENV_PATH="$(dirname "$(readlink -qe "${BASH_SOURCE[0]}")")/../../versions.env"

if [ -f "$VERSIONS_ENV_PATH" ]; then
	# shellcheck disable=SC1090
	. "$VERSIONS_ENV_PATH"
else
	echo -e "Error: versions.env file not found at $VERSIONS_ENV_PATH"
	exit 1
fi

# Help message function
usage() {
	echo "Usage: $0 [options]"
	echo "Options:"
	echo "  -v <version>    Specify the FFmpeg version to build (default is $FFMPEG_VERSION)"
	echo "  -g              Enable GPU direct mode during compilation"
	echo "  -j              Enable SVT-JPEG-XS support during compilation"
	echo "  -h              Display this help and exit"
}

# Parse command-line options
while getopts ":v:hgj" opt; do
	case "${opt}" in
	v)
		FFMPEG_VERSION=${OPTARG}
		;;
	g)
		enable_gpu=true
		;;
	j)
		enable_jpegxs=true
		;;
	h)
		usage
		exit 0
		;;
	\?)
		echo "Invalid option: -$OPTARG" >&2
		usage
		exit 1
		;;
	:)
		echo "Option -$OPTARG requires an argument." >&2
		exit 1
		;;
	esac
done

build_openh264() {
	if command -v pkg-config >/dev/null 2>&1; then
		if pkg-config --exists openh264; then
			echo "openh264 is already installed (detected by pkg-config). Skipping build."
			return
		fi
	else
		echo "Warning: pkg-config not found. Skipping openh264 installation check."
	fi

	if [ -d "openh264-openh264v2.4.0" ]; then
		echo "openh264v2.4.0 directory already exists. Removing it to ensure a clean build."
		rm -rf openh264-openh264v2.4.0
	fi

	wget https://github.com/cisco/openh264/archive/refs/heads/openh264v2.4.0.zip
	unzip openh264v2.4.0.zip && rm -f openh264v2.4.0.zip
	cd openh264-openh264v2.4.0
	if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		make -j "$(nproc)" PREFIX="${MTL_INSTALL_PREFIX}"
		make install PREFIX="${MTL_INSTALL_PREFIX}"
	else
		make -j "$(nproc)"
		sudo make install
		sudo ldconfig
	fi
	cd ../
}

build_ffmpeg() {
	pushd "$script_path"
	if [ -d "FFmpeg-release-${FFMPEG_VERSION}" ]; then
		echo "FFmpeg directory already exists. Removing it to ensure a clean build."
		rm -rf "FFmpeg-release-${FFMPEG_VERSION}"
	fi

	wget "https://github.com/FFmpeg/FFmpeg/archive/refs/heads/release/${FFMPEG_VERSION}.zip"
	unzip "${FFMPEG_VERSION}.zip" && rm -f "${FFMPEG_VERSION}.zip"

	pushd "./FFmpeg-release-${FFMPEG_VERSION}"
	cp -f "$script_path"/mtl_* ./libavdevice/

	for patch_file in "$script_path"/"$FFMPEG_VERSION"/*.patch; do
		if [ -f "$patch_file" ]; then
			echo "Applying patch: $(basename "$patch_file")"
			patch -p1 <"$patch_file"
		fi
	done

	# Use bash array to pass extra configuration flags to avoid shellcheck SC2086 word-splitting warnings.
	extra_config_flags=()

	if [ "$enable_gpu" = true ]; then
		echo "Building with MTL_GPU_DIRECT_ENABLED"
		extra_config_flags+=("--extra-cflags=-DMTL_GPU_DIRECT_ENABLED")
	fi

	local jpegxs_repo="${SVT_JPEG_XS_REPO:-}"
	if [ -z "$jpegxs_repo" ]; then
		if [ -d "${script_path}/../../.github/scripts/SVT-JPEG-XS" ]; then
			jpegxs_repo="${script_path}/../../.github/scripts/SVT-JPEG-XS"
		fi
	fi

	if { [ "${FFMPEG_ENABLE_SVT_JPEG_XS:-0}" == "1" ] || [ "$enable_jpegxs" = true ]; }; then
		echo "Integrating SVT-JPEG-XS support into FFmpeg..."
		if [ ! -d "${jpegxs_repo}" ] && ! wget -q "https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/refs/tags/${SVT_JPEG_XS_VER}.tar.gz" -O "${script_path}/SVT-JPEG-XS.tar.gz"; then
			wget -q "https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/${SVT_JPEG_XS_VER}.tar.gz" -O "${script_path}/SVT-JPEG-XS.tar.gz"

			mkdir -p "${jpegxs_repo}"
			tar -xzf "${script_path}/SVT-JPEG-XS.tar.gz" -C "${jpegxs_repo}" --strip-components=1
			rm -f "${script_path}/SVT-JPEG-XS.tar.gz"
		fi

		if [ -f "${jpegxs_repo}/ffmpeg-plugin/libsvtjpegxsenc.c" ] || [ -f "${jpegxs_repo}/ffmpeg-plugin/libsvtjpegxsdec.c" ]; then
			cp -f "${jpegxs_repo}/ffmpeg-plugin/libsvtjpegxs"* libavcodec/
		fi

		local patch_dir="${jpegxs_repo}/ffmpeg-plugin/${FFMPEG_VERSION}"
		if [ ! -d "$patch_dir" ]; then
			patch_dir="${jpegxs_repo}/ffmpeg-plugin"
		fi

		for patch_file in "${patch_dir}"/*.patch; do
			if [ -f "$patch_file" ]; then
				echo "Applying SVT-JPEG-XS patch: $(basename "$patch_file")"
				patch -p1 <"$patch_file"
			fi
		done
		extra_config_flags+=("--enable-libsvtjpegxs")
	fi

	if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		# Ensure FFmpeg can find libraries installed in its own prefix (e.g. openh264)
		export PKG_CONFIG_PATH="${MTL_INSTALL_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
		export LD_LIBRARY_PATH="${MTL_INSTALL_PREFIX}/lib:${LD_LIBRARY_PATH:-}"
		./configure --prefix="${MTL_INSTALL_PREFIX}" --enable-shared --disable-static --enable-pic --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl --extra-ldflags="-Wl,-rpath,${MTL_INSTALL_PREFIX}/lib" "${extra_config_flags[@]}"
		make -j "$(nproc)"
		make install
	else
		./configure --enable-shared --disable-static --enable-pic --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl "${extra_config_flags[@]}"
		make -j "$(nproc)"
		sudo make install
		sudo ldconfig
	fi
	popd
	popd
}

build_openh264
build_ffmpeg

echo "Building ffmpeg $FFMPEG_VERSION plugin is finished"
