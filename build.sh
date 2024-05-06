#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

user=$(whoami)

function usage()
{
    echo "Usage: $0 [debug/debugoptimized/plain/release]"
    exit 0
}

buildtype=release
enable_asan=false
enable_tap=false
enable_usdt=true

if [ -n "$MTL_BUILD_ENABLE_ASAN" ];  then
    if [ "$MTL_BUILD_ENABLE_ASAN" == "true" ]; then
        enable_asan=true
        buildtype=debug # use debug build as default for asan
        echo "Enable asan check."
    fi
fi

if [ -n "$MTL_BUILD_ENABLE_TAP" ];  then
    if [ "$MTL_BUILD_ENABLE_TAP" == "true" ]; then
        enable_tap=true
        echo "Enable tap"
    fi
fi

if [ -n "$MTL_BUILD_DISABLE_USDT" ];  then
    if [ "$MTL_BUILD_DISABLE_USDT" == "true" ]; then
        enable_usdt=false
        echo "Disable USDT"
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
LIB_BUILD_DIR=${WORKSPACE}/build
APP_BUILD_DIR=${WORKSPACE}/build/app
TEST_BUILD_DIR=${WORKSPACE}/build/tests
PLUGINS_BUILD_DIR=${WORKSPACE}/build/plugins
LD_PRELOAD_BUILD_DIR=${WORKSPACE}/build/ld_preload
MANAGER_BUILD_DIR=${WORKSPACE}/build/manager
RDMA_BUILD_DIR=${WORKSPACE}/build/rdma

# build lib
meson setup "${LIB_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan" -Denable_tap="$enable_tap" -Denable_usdt="$enable_usdt"
pushd "${LIB_BUILD_DIR}"
ninja
if [ "$user" == "root" ] || [ "$OS" == "Windows_NT" ]; then
    ninja install
else
    sudo ninja install
fi
popd

# build rdma lib
if [ "$OS" != "Windows_NT" ]; then
pushd rdma/
meson setup "${RDMA_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${RDMA_BUILD_DIR}"
ninja
if [ "$user" == "root" ]; then
    ninja install
else
    sudo ninja install
fi
popd
fi

# build app
pushd app/
meson setup "${APP_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${APP_BUILD_DIR}"
ninja
popd

# build tests
pushd tests/
meson setup "${TEST_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${TEST_BUILD_DIR}"
ninja
popd

# build plugins
pushd plugins/
meson setup "${PLUGINS_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${PLUGINS_BUILD_DIR}"
ninja
if [ "$user" == "root" ] || [ "$OS" == "Windows_NT" ]; then
    ninja install
else
    sudo ninja install
fi
popd

# build ld_preload
if [ "$OS" != "Windows_NT" ]; then
pushd ld_preload/
meson setup "${LD_PRELOAD_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${LD_PRELOAD_BUILD_DIR}"
ninja
if [ "$user" == "root" ]; then
    ninja install
else
    sudo ninja install
fi
popd
fi

# build mtl_manager
if [ "$OS" != "Windows_NT" ]; then
pushd manager/
meson setup "${MANAGER_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${MANAGER_BUILD_DIR}"
ninja
if [ "$user" == "root" ]; then
    ninja install
else
    sudo ninja install
fi
popd
fi