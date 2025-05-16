#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
cd "${script_folder}"

if [ -n "$1" ]; then
	dpdk_ver=$1
else
	# default to latest 25.03
	dpdk_ver=25.03
fi

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	echo "DPDK version: $dpdk_ver"

	if [ ! -d "dpdk" ]; then
		# check out dpdk code
		echo "Clone DPDK source code"
		git clone https://github.com/DPDK/dpdk.git
	fi
	cd dpdk
	git checkout v"$dpdk_ver"
	git switch -c v"$dpdk_ver"

	# apply the patches
	git am ../../patches/dpdk/"$dpdk_ver"/*.patch

	# build and install dpdk now
	meson build
	ninja -C build
	cd build
	sudo ninja install
fi