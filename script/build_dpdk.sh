#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
# shellcheck disable=SC1091
. "${script_folder}/common.sh"
cd "${script_folder}" || exit 1

set -x

if [ -n "$1" ]; then
	DPDK_VER=$1
fi
dpdk_folder="dpdk_${DPDK_VER}"

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	echo "DPDK version: $DPDK_VER"

	if [ ! -d "$dpdk_folder" ]; then
		# check out dpdk code
		echo "Clone DPDK source code"
		git clone https://github.com/DPDK/dpdk.git "$dpdk_folder"
	fi

	cd "$dpdk_folder" || exit 1
	git checkout v"$DPDK_VER"
	if git switch -c v"$DPDK_VER"_kahawai_build 2>/dev/null; then
		git am ../../patches/dpdk/"$DPDK_VER"/*.patch
	else
		echo "Branch v${DPDK_VER}_kahawai_build already exists."
		echo "Skipping patch application assuming it has been applied before."
	fi

	# build and install dpdk now
	meson build
	ninja -C build
	cd build
	sudo ninja install
fi
