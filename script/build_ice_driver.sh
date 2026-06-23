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
	# Skip rebuild if the correct Kahawai ICE version is already loaded.
	# Set FORCE_ICE_REBUILD=1 to override.
	if [ "${FORCE_ICE_REBUILD:-0}" != "1" ]; then
		if sudo modinfo ice 2>/dev/null | grep -qEi "^version:[[:space:]]*Kahawai_${ICE_VER}"; then
			echo "ICE driver version ${ICE_VER} (Kahawai) is already installed. Skipping rebuild."
			exit 0
		fi
	fi

	archive_name="ice-${ICE_VER}.tar.gz"
	echo "Building e810 driver version: $ICE_VER form mirror $ICE_DMID"

	IS_GITHUB_ARCHIVE=0
	# Check if local archive already exists and is a valid compressed file
	if [ -f "$archive_name" ] && gzip -t "$archive_name" >/dev/null 2>&1; then
		echo "Found valid local archive $archive_name, skipping download."
		# Check if the existing local file is actually a GitHub download
		if tar -tzf "$archive_name" | grep -q "^ethernet-linux-ice"; then
			IS_GITHUB_ARCHIVE=1
		fi
	else
		rm -f "$archive_name"
		echo "Downloading ICE driver of version ${ICE_VER}..."
		wget "https://downloadmirror.intel.com/${ICE_DMID}/${archive_name}" -O "$archive_name" || true
		if [ ! -f "$archive_name" ] || ! gzip -t "$archive_name" >/dev/null 2>&1; then
			echo "Intel mirror download failed or was blocked by AWS WAF. Trying GitHub fallback..."
			rm -f "$archive_name"
			wget "https://github.com/intel/ethernet-linux-ice/archive/refs/tags/v${ICE_VER}.tar.gz" -O "$archive_name" || true
			if [ -f "$archive_name" ] && gzip -t "$archive_name" >/dev/null 2>&1; then
				echo "Successfully downloaded driver from GitHub fallback."
				IS_GITHUB_ARCHIVE=1
			else
				echo "Error: Failed to download a valid $archive_name from both Intel mirror and GitHub."
				echo "This is likely caused by corporate proxy blockage or firewall settings."
				rm -f "$archive_name"
				exit 1
			fi
		fi
	fi

	if [ -d "ice-${ICE_VER}" ]; then
		echo "ice-${ICE_VER} directory already exists, please remove it first"
		exit 1
	fi

	tar xvzf "$archive_name"

	rm -f "$archive_name"

	if [ "${IS_GITHUB_ARCHIVE}" -eq 1 ]; then
		if [ -d "ethernet-linux-ice-${ICE_VER}" ]; then
			mv "ethernet-linux-ice-${ICE_VER}" "ice-${ICE_VER}"
		fi
	fi

	if [ ! -d "ice-${ICE_VER}" ]; then
		echo "Failed to extract $archive_name"
		exit 1
	fi

	cd "ice-${ICE_VER}"

	for patch_file in ../../patches/ice_drv/"${ICE_VER}"/*.patch; do
		patch -p1 -i "$patch_file"
	done

	cd src
	make -j
	sudo make install
	sudo rmmod irdma || echo "irdma not loaded"
	sudo rmmod ice
	sudo modprobe ice

	cd "$script_folder" || exit 1
	echo "Removing downloaded ice-${ICE_VER} sources."
	rm -rf "ice-${ICE_VER}"
fi
