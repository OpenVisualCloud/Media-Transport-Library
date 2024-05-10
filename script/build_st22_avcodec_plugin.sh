#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

function usage() {
	echo "Usage: $0 [debug]"
	exit 0
}

buildtype=release

if [ -n "$1" ]; then
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
ST22_AVCODEC_PLUGIN_BUILD_DIR=${WORKSPACE}/build/st22_avcodec_plugin

# build st22 avcodec plugin
pushd plugins/st22_avcodec/
meson "${ST22_AVCODEC_PLUGIN_BUILD_DIR}" -Dbuildtype="$buildtype"
popd
pushd "${ST22_AVCODEC_PLUGIN_BUILD_DIR}"
ninja
sudo ninja install
popd
