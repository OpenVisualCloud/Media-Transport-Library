#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

set -e

function usage()
{
    echo "Usage: $0 [debug/debugoptimized/plain/release]"
    exit 0
}

buildtype=release
enable_asan=false

if [ -n "$MTL_BUILD_ENABLE_ASAN" ];  then
    if [ "$MTL_BUILD_ENABLE_ASAN" == "true" ]; then
        enable_asan=true
        buildtype=debug # use debug build as default for asan
        echo "Enable asan check."
    fi
fi

if [ -n "$1" ];  then
    case $1 in
      "debug")
           buildtype=debug
           enable_asan=true
           ;;
      "debugonly")
           buildtype=debug
           ;;
      "debugoptimized")
           buildtype=debugoptimized
           ;;
      "plain")
           buildtype=plain
           ;;
      "release")
           buildtype=release
           ;;
       *)
           usage
           ;;
    esac
fi

WORKSPACE=$PWD
APP_BUILD_DIR=${WORKSPACE}/build/app

# build app
pushd app/
meson setup "${APP_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${APP_BUILD_DIR}"
ninja
popd
