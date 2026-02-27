#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

user=$(whoami)

function usage() {
	echo "Usage: $0 [debug|debugonly|debugoptimized|plain|release] [enable_fuzzing]"
	exit 0
}

buildtype=release
enable_asan=false
enable_tap=false
enable_usdt=true
enable_fuzzing=false

: "${MTL_BUILD_ENABLE_ASAN:=false}"
: "${MTL_BUILD_ENABLE_TAP:=false}"
: "${MTL_BUILD_DISABLE_USDT:=false}"
: "${MTL_BUILD_ENABLE_FUZZING:=false}"

if [ "$MTL_BUILD_ENABLE_ASAN" == "true" ]; then
	enable_asan=true
	buildtype=debug # use debug build as default for asan
	echo "Enable asan check."
fi

if [ "$MTL_BUILD_ENABLE_TAP" == "true" ]; then
	enable_tap=true
	echo "Enable tap"
fi

if [ "$MTL_BUILD_DISABLE_USDT" == "true" ]; then
	enable_usdt=false
	echo "Disable USDT"
fi

if [ "$MTL_BUILD_ENABLE_FUZZING" == "true" ]; then
	enable_fuzzing=true
	echo "Enable fuzzers"
fi

while [ $# -gt 0 ]; do
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
	"enable_fuzzing" | "--enable-fuzzing")
		enable_fuzzing=true
		echo "Enable fuzzers"
		;;
	*)
		usage
		;;
	esac
	shift
done

WORKSPACE=$PWD
LIB_BUILD_DIR=${WORKSPACE}/build
APP_BUILD_DIR=${WORKSPACE}/build/app
TEST_BUILD_DIR=${WORKSPACE}/build/tests
PLUGINS_BUILD_DIR=${WORKSPACE}/build/plugins
LD_PRELOAD_BUILD_DIR=${WORKSPACE}/build/ld_preload
MANAGER_BUILD_DIR=${WORKSPACE}/build/manager
RXTXAPP_BUILD_DIR=${WORKSPACE}/tests/tools/RxTxApp/build

prefix_arg=""
if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
	prefix_arg="--prefix=$MTL_INSTALL_PREFIX"
fi

do_install() {
	if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		ninja install
	elif [ "$user" == "root" ] || [ "$OS" == "Windows_NT" ]; then
		ninja install
	else
		sudo ninja install
	fi
}

# build lib
meson setup "${LIB_BUILD_DIR}" $prefix_arg -Dbuildtype="$buildtype" -Denable_asan="$enable_asan" -Denable_tap="$enable_tap" -Denable_usdt="$enable_usdt" -Denable_fuzzing="$enable_fuzzing"
pushd "${LIB_BUILD_DIR}"
ninja
do_install
popd

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
meson setup "${PLUGINS_BUILD_DIR}" $prefix_arg -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${PLUGINS_BUILD_DIR}"
ninja
do_install
popd

# build ld_preload
if [ "$OS" != "Windows_NT" ]; then
	pushd ld_preload/
	meson setup "${LD_PRELOAD_BUILD_DIR}" $prefix_arg -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
	popd
	pushd "${LD_PRELOAD_BUILD_DIR}"
	ninja
	do_install
	popd
fi

# build mtl_manager
if [ "$OS" != "Windows_NT" ]; then
	pushd manager/
	meson setup "${MANAGER_BUILD_DIR}" $prefix_arg -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
	popd
	pushd "${MANAGER_BUILD_DIR}"
	ninja
	do_install
	popd
fi

# build RxTxApp
pushd tests/tools/RxTxApp/
meson setup "${RXTXAPP_BUILD_DIR}" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
popd
pushd "${RXTXAPP_BUILD_DIR}"
ninja
popd
