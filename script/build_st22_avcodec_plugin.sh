#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}

cd "${script_folder}"

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
# When MTL_PLUGIN_PREFIX is set, install into that prefix (no sudo, used for the
# CI .local_install cache). Otherwise fall back to a system-wide install.
meson_prefix_args=()
if [ -n "${MTL_PLUGIN_PREFIX:-}" ]; then
	meson_prefix_args+=(--prefix "${MTL_PLUGIN_PREFIX}")
fi

pushd "${script_folder}/../plugins/st22_avcodec/"
meson "${ST22_AVCODEC_PLUGIN_BUILD_DIR}" -Dbuildtype="$buildtype" "${meson_prefix_args[@]}"
popd
pushd "${ST22_AVCODEC_PLUGIN_BUILD_DIR}"
ninja
if [ -n "${MTL_PLUGIN_PREFIX:-}" ]; then
	ninja install
else
	sudo ninja install
fi
popd
