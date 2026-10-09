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
OBS_PLUGIN_BUILD_DIR=${WORKSPACE}/build/obs_plugin

# build obs plugin
pushd ecosystem/obs_mtl/linux-mtl
meson "${OBS_PLUGIN_BUILD_DIR}" -Dbuildtype="$buildtype"
popd
pushd "${OBS_PLUGIN_BUILD_DIR}"
ninja
sudo ninja install
popd
