#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

function usage()
{
    echo "Usage: $0 [debug/debugoptimized/plain/release]"
    exit 0
}

buildtype=release
disable_pcapng=false

if [ -n "$ST_BUILD_DISABLE_PCAPNG" ];  then
    if [ $ST_BUILD_DISABLE_PCAPNG == "true" ]; then
        disable_pcapng=true
        echo "Disable pcapng function."
    fi
fi

if [ -n "$1" ];  then
    case $1 in
      "debug")
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

# build lib
meson ${LIB_BUILD_DIR} -Dbuildtype=$buildtype -Ddisable_pcapng=$disable_pcapng
pushd ${LIB_BUILD_DIR}
ninja
sudo ninja install
popd

# build app
pushd app/
meson ${APP_BUILD_DIR} -Dbuildtype=$buildtype
popd
pushd ${APP_BUILD_DIR}
ninja
popd

# build tests
pushd tests/
meson ${TEST_BUILD_DIR} -Dbuildtype=$buildtype
popd
pushd ${TEST_BUILD_DIR}
ninja
popd

# build plugins
pushd plugins/
meson ${PLUGINS_BUILD_DIR} -Dbuildtype=$buildtype
popd
pushd ${PLUGINS_BUILD_DIR}
ninja
sudo ninja install
popd
