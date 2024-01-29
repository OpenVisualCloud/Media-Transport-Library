#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

set -e

build_openh264(){
    rm openh264 -rf
    git clone https://github.com/cisco/openh264.git -b openh264v2.4.0
    cd openh264
    make -j "$(nproc)"
    sudo make install
    sudo ldconfig
    cd ../
}

build_ffmpeg(){
    rm FFmpeg -rf
    git clone https://github.com/FFmpeg/FFmpeg.git -b release/6.1
    cd FFmpeg
    cp -f ../mtl_* ./libavdevice/
    git am ../0001-avdevice-add-mtl-in-out-dev-support.patch
    ./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl
    make -j "$(nproc)"
    sudo make install
    sudo ldconfig
    cd ../
}

build_openh264
build_ffmpeg

echo "Building ffmpeg plugin is finished"
