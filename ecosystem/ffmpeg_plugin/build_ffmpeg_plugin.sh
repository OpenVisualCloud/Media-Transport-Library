#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation

set -e

# Default values
ffmpeg_ver="7.0"
enable_gpu=false
script_path="$(dirname "$(readlink -f "$0")")"

# Help message function
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -v <version>    Specify the FFmpeg version to build (default is 7.0)"
    echo "  -g              Enable GPU direct mode during compilation"
    echo "  -h              Display this help and exit"
}

# Parse command-line options
while getopts ":v:hg" opt; do
    case "${opt}" in
        v)
            ffmpeg_ver=${OPTARG}
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
    rm -rf openh264
    git clone https://github.com/cisco/openh264.git
    cd openh264
    git checkout openh264v2.4.0
    make -j "$(nproc)"
    sudo make install
    sudo ldconfig
    cd ../
}

build_ffmpeg() {
    rm -rf FFmpeg
    git clone https://github.com/FFmpeg/FFmpeg.git
    cd FFmpeg
    git checkout release/"$ffmpeg_ver"
    cp -f "$script_path"/mtl_* ./libavdevice/
    git am "$script_path"/"$ffmpeg_ver"/*.patch

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

echo "Building ffmpeg $ffmpeg_ver plugin is finished"
