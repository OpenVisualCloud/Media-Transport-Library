#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -e

download_mirror=843928
script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
set -x
cd "${script_folder}"

if [ -n "$1" ]; then
	ice_driver_ver=$1
else
	ice_driver_ver=1.16.3
fi

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	echo "Building e810 driver version: $ice_driver_ver form mirror $download_mirror"

	wget "https://downloadmirror.intel.com/${download_mirror}/ice-${ice_driver_ver}.tar.gz"
	if [ ! -f "ice-${ice_driver_ver}.tar.gz" ]; then
		echo "Failed to download ice-${ice_driver_ver}.tar.gz"
		exit 1
	fi

	if [ -d "ice-${ice_driver_ver}" ]; then
		echo "ice-${ice_driver_ver} already exists, please remove it first"
		exit 1
	fi

	tar xvzf "ice-${ice_driver_ver}.tar.gz"

	if [ ! -d "ice-${ice_driver_ver}" ]; then
		echo "Failed to extract ice-${ice_driver_ver}.tar.gz"
		exit 1
	fi

	cd "ice-${ice_driver_ver}"

	git init
	git add .
	git commit -m "init version ${ice_driver_ver}"
	git am ../../patches/ice_drv/"${ice_driver_ver}"/*.patch

	cd src
	make
	sudo make install
	sudo rmmod ice
	sudo modprobe ice
	cd -
fi
