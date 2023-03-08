#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

user=`whoami`

function usage()
{
    echo "Usage: $0 [debug/debugoptimized/plain/release]"
    exit 0
}

buildtype=release
disable_pcapng=false
enable_asan=false
enable_kni=false
enable_tap=false

if [ -n "$MTL_BUILD_DISABLE_PCAPNG" ];  then
    if [ "$MTL_BUILD_DISABLE_PCAPNG" == "true" ]; then
        disable_pcapng=true
        echo "Disable pcapng function."
    fi
fi

if [ -n "$MTL_BUILD_ENABLE_ASAN" ];  then
    if [ "$MTL_BUILD_ENABLE_ASAN" == "true" ]; then
        enable_asan=true
        buildtype=debug # use debug build as default for asan
        echo "Enable asan check."
    fi
fi

if [ -n "$MTL_BUILD_ENABLE_KNI" ];  then
    if [ "$MTL_BUILD_ENABLE_KNI" == "true" ]; then
        enable_kni=true
        echo "Enable kni"
    fi
fi

if [ -n "$MTL_BUILD_ENABLE_TAP" ];  then
    if [ "$MTL_BUILD_ENABLE_TAP" == "true" ]; then
        enable_tap=true
        echo "Enable tap"
    fi
fi

if [ -n "$1" ];  then
    case $1 in
      "debug")
           buildtype=debug
           enable_asan=true
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
LIB_BUILD_DIR=${WORKSPACE}/build
APP_BUILD_DIR=${WORKSPACE}/build/app
TEST_BUILD_DIR=${WORKSPACE}/build/tests
PLUGINS_BUILD_DIR=${WORKSPACE}/build/plugins

# build lib
meson "${LIB_BUILD_DIR}" -Dbuildtype="$buildtype" -Ddisable_pcapng="$disable_pcapng" -Denable_asan="$enable_asan" -Denable_kni="$enable_kni" -Denable_tap="$enable_tap"
pushd "${LIB_BUILD_DIR}"
ninja
if [ $user == "root" ]; then
    ninja install
else
    sudo ninja install
fi
popd

# build app
pushd app/
meson "${APP_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${APP_BUILD_DIR}"
ninja
popd

# build tests
pushd tests/
meson "${TEST_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${TEST_BUILD_DIR}"
ninja
popd

# build plugins
pushd plugins/
meson "${PLUGINS_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${PLUGINS_BUILD_DIR}"
ninja
if [ $user == "root" ]; then
    ninja install
else
    sudo ninja install
fi
popd
