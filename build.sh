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
      "debugoptimized")
           buildtype=debugoptimized
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
LIB_BUILD_DIR=${WORKSPACE}/build
APP_BUILD_DIR=${WORKSPACE}/build/app
TEST_BUILD_DIR=${WORKSPACE}/build/tests
PLUGINS_BUILD_DIR=${WORKSPACE}/build/plugins

# build lib
meson ${LIB_BUILD_DIR} -Dbuildtype=$buildtype
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
