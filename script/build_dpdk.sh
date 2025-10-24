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

show_help() {
	cat <<EOF
Usage: $script_name [OPTIONS] [DPDK_VERSION]

Build and install DPDK with MTL patches.

OPTIONS:
	-f			Force rebuild (removes existing dpdk folder if present)
	-h			Show this help message
	-v VERSION	Specify DPDK version to build

	EXAMPLES:
		$script_name					# Build default DPDK version
		$script_name -v					# Build specific DPDK version
		$script_name -f -v 23.11		# Force rebuild of DPDK 23.11
EOF
}

FORCE=0
while getopts "fhv:" opt; do
	case $opt in
	f)
		FORCE=1
		;;
	v)
		DPDK_VER="$OPTARG"
		;;
	h)
		show_help
		exit 0
		;;
	*)
		show_help
		exit 1
		;;
	esac
done
shift $((OPTIND - 1))

dpdk_folder="dpdk-${DPDK_VER}"

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	echo "DPDK version: $DPDK_VER"

	if [ $FORCE -eq 1 ] && [ -d "$dpdk_folder" ]; then
		echo "Force rebuild enabled. Removing existing '$dpdk_folder' directory."
		rm -rf "$dpdk_folder"
	fi

	if [ ! -d "$dpdk_folder" ]; then
		echo "Clone DPDK source code"
		wget https://github.com/DPDK/dpdk/archive/refs/tags/v"${DPDK_VER}".zip
		unzip v"${DPDK_VER}".zip

		cd "$dpdk_folder" || exit 1
		for patch_file in ../../patches/dpdk/"$DPDK_VER"/*.patch; do
			patch -p1 -i "$patch_file"
		done
	else
		echo "DPDK source code already exists."
		echo "To rebuild, please remove the '$dpdk_folder' directory and run this script again."
		exit 0
	fi

	echo Build and install DPDK now
	meson build
	ninja -C build
	cd build
	sudo ninja install
fi
