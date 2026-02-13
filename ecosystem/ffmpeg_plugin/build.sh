#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation

set -e

enable_gpu=false
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
	echo "  -h              Display this help and exit"
}

# Parse command-line options
while getopts ":v:hg" opt; do
	case "${opt}" in
	v)
		FFMPEG_VERSION=${OPTARG}
		;;
	g)
		enable_gpu=true
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
	make -j "$(nproc)"
	sudo make install
	sudo ldconfig
	cd ../
}

build_ffmpeg() {
	if [ -d "FFmpeg-release-${FFMPEG_VERSION}" ]; then
		echo "FFmpeg directory already exists. Removing it to ensure a clean build."
		rm -rf "FFmpeg-release-${FFMPEG_VERSION}"
	fi

	wget "https://github.com/FFmpeg/FFmpeg/archive/refs/heads/release/${FFMPEG_VERSION}.zip"
	unzip "${FFMPEG_VERSION}.zip" && rm -f "${FFMPEG_VERSION}.zip"

	cd "FFmpeg-release-${FFMPEG_VERSION}"
	cp -f "$script_path"/mtl_* ./libavdevice/

	for patch_file in "$script_path"/"$FFMPEG_VERSION"/*.patch; do
		if [ -f "$patch_file" ]; then
			echo "Applying patch: $(basename "$patch_file")"
			patch -p1 <"$patch_file"
		fi
	done

	if [ "$enable_gpu" = true ]; then
		echo "Building with MTL_GPU_DIRECT_ENABLED"
		extra_config_flags="--extra-cflags=-DMTL_GPU_DIRECT_ENABLED"
	else
		extra_config_flags=""
	fi

	./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl $extra_config_flags
	make -j "$(nproc)"
	sudo make install
	sudo ldconfig
	cd ../
}

build_openh264
build_ffmpeg

echo "Building ffmpeg $FFMPEG_VERSION plugin is finished"
