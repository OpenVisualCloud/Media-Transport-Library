#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -e
VERSIONS_ENV_PATH="$(dirname "$(readlink -qe "${BASH_SOURCE[0]}")")/../versions.env"

if [ -f "$VERSIONS_ENV_PATH" ]; then
	# shellcheck disable=SC1090
	. "$VERSIONS_ENV_PATH"
else
	echo -e "Error: versions.env file not found at $VERSIONS_ENV_PATH"
	exit 1
fi

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
set -x
cd "${script_folder}"

if [ -n "$1" ]; then
	ICE_VER=$1
fi

if [ -n "$2" ]; then
	ICE_DMID=$2
fi

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	echo "Building e810 driver version: $ICE_VER form mirror $ICE_DMID"

	wget "https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz"
	if [ ! -f "ice-${ICE_VER}.tar.gz" ]; then
		echo "Failed to download ice-${ICE_VER}.tar.gz"
		exit 1
	fi

	if [ -d "ice-${ICE_VER}" ]; then
		echo "ice-${ICE_VER} directory already exists, please remove it first"
		exit 1
	fi

	tar xvzf "ice-${ICE_VER}.tar.gz"

	if [ ! -d "ice-${ICE_VER}" ]; then
		echo "Failed to extract ice-${ICE_VER}.tar.gz"
		exit 1
	fi

	cd "ice-${ICE_VER}"

	git init
	git add .
	git commit -m "init version ${ICE_VER}"
	git am ../../patches/ice_drv/"${ICE_VER}"/*.patch

	cd src
	make -j
	sudo make install
	sudo rmmod irdma || echo "irdma not loaded"
	sudo rmmod ice
	sudo modprobe ice
	cd -
fi
