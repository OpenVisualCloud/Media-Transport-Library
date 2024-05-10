#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

set -e

if [ -n "$1" ]; then
	ffmpeg_ver=$1
else
	# default to latest 6.1
	ffmpeg_ver=6.1
fi

build_openh264() {
	rm openh264 -rf
	git clone https://github.com/cisco/openh264.git
	cd openh264
	git checkout openh264v2.4.0
	make -j "$(nproc)"
	sudo make install
	sudo ldconfig
	cd ../
}

build_ffmpeg() {
	rm FFmpeg -rf
	git clone https://github.com/FFmpeg/FFmpeg.git
	cd FFmpeg
	git checkout release/"$ffmpeg_ver"
	cp -f ../mtl_* ./libavdevice/
	git am ../"$ffmpeg_ver"/*.patch
	./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl
	make -j "$(nproc)"
	sudo make install
	sudo ldconfig
	cd ../
}

build_openh264
build_ffmpeg

echo "Building ffmpeg $ffmpeg_ver plugin is finished"
