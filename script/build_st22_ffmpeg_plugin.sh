#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

function usage()
{
    echo "Usage: $0 [debug]"
    exit 0
}

buildtype=release

if [ -n "$1" ];  then
    case $1 in
      "debug")
           buildtype=debug
           ;;
      "plain")
           buildtype=plain
           ;;
       *)
           usage
           ;;
    esac
fi

WORKSPACE=$PWD
ST22_FFMPEG_PLUGIN_BUILD_DIR=${WORKSPACE}/build/st22_ffmpeg_plugin

# build st22 ffmpeg plugin
pushd plugins/st22_ffmpeg/
meson "${ST22_FFMPEG_PLUGIN_BUILD_DIR}" -Dbuildtype="$buildtype"
popd
pushd "${ST22_FFMPEG_PLUGIN_BUILD_DIR}"
ninja
sudo ninja install
popd
