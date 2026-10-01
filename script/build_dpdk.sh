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

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc meson ninja-build patch pkg-config python3 python3-pyelftools
	libnuma-dev unzip wget

OPTIONS:
	-f			Force rebuild (reuses existing DPDK source if present)
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

# Check if the correct MTL-patched DPDK is already installed via pkg-config.
# Since 26.03, MTL patches start the version with "${DPDK_VER}.${DPDK_MTL_MINOR_VER}_mtl_".
# Older versions use a plain version string match.
dpdk_is_installed() {
	local installed_ver
	installed_ver=$(pkg-config --modversion libdpdk 2>/dev/null) || return 1
	[ -z "$installed_ver" ] && return 1

	local mtl_tag_since="26.03"
	if printf '%s\n' "$mtl_tag_since" "$DPDK_VER" | sort -V | head -n1 | grep -qx "$mtl_tag_since"; then
		[[ "$installed_ver" == "${DPDK_VER}.${DPDK_MTL_MINOR_VER}_mtl_"* ]]
	else
		[[ "$installed_ver" == "$DPDK_VER" ]]
	fi
}

check_build_packages() {
	local candidate missing=() item

	for item in cc:gcc meson:meson ninja:ninja-build patch:patch pkg-config:pkg-config python3:python3 unzip:unzip wget:wget; do
		command_exists "${item%%:*}" || missing+=("${item%%:*}")
	done
	DPDK_PYTHON=""
	for candidate in "$(command -v python3 2>/dev/null || true)" /usr/bin/python3 /usr/local/bin/python3; do
		if [ -x "$candidate" ] && "$candidate" -c 'import elftools' 2>/dev/null; then
			DPDK_PYTHON="$candidate"
			break
		fi
	done
	if command_exists python3 && [ -z "$DPDK_PYTHON" ]; then
		missing+=("${PYELFTOOLS_PACKAGE:-Python elftools module}")
	fi
	if ! command_exists pkg-config || ! pkg-config --exists numa; then
		missing+=("${NUMA_DEVEL_PACKAGE:-NUMA development files}")
	fi

	if [ "${#missing[@]}" -ne 0 ]; then
		if [ "$FORCE" -eq 1 ]; then
			log_warning "Required packages are missing. Continuing because force mode is enabled:"
			printf '  %s\n' "${missing[@]}" >&2
			return 0
		fi
		log_error "Cannot build DPDK. Required packages are missing:"
		printf '  %s\n' "${missing[@]}" >&2
		return 1
	fi
}

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	log_info "Attempting to install DPDK version: ${DPDK_VER}.${DPDK_MTL_MINOR_VER}"

	# Skip rebuild if the correct version is already installed system-wide.
	# Local-prefix installs always rebuild. Use -f to force.
	if [ $FORCE -eq 0 ] && [ -z "${MTL_INSTALL_PREFIX:-}" ] && dpdk_is_installed; then
		log_success "DPDK already installed ($(pkg-config --modversion libdpdk)). Skipping rebuild."
		exit 0
	fi

	if [ $FORCE -eq 0 ] && [ -d "$dpdk_folder" ]; then
		log_warning "DPDK source code already exists."
		log_info "To rebuild, remove the '$dpdk_folder' directory and run this script again."
		exit 0
	fi

	check_build_packages || exit 1
	if [ $FORCE -eq 1 ] && [ -d "$dpdk_folder" ]; then
		log_warning "Force rebuild enabled. Reusing existing '$dpdk_folder' directory."
	fi

	if [ ! -d "$dpdk_folder" ]; then
		log_info "Clone DPDK source code"
		archive_name="v${DPDK_VER}.zip"
		rm -f "$archive_name"
		wget "https://github.com/DPDK/dpdk/archive/refs/tags/${archive_name}" -O "$archive_name"
		unzip "$archive_name"
		rm -f "$archive_name"

		cd "$dpdk_folder" || exit 1
		for patch_file in ../../patches/dpdk/"$DPDK_VER"/*.patch; do
			patch -p1 -i "$patch_file"
		done
	else
		cd "$dpdk_folder" || exit 1
	fi

	log_info "Build and install DPDK now"
	: "${MTL_PREFIX_ARGS:=}"
	if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		MTL_PREFIX_ARGS="--prefix=$MTL_INSTALL_PREFIX"
	fi
	PATH="$(dirname "$DPDK_PYTHON"):$PATH" meson build ${MTL_PREFIX_ARGS:+"$MTL_PREFIX_ARGS"}
	ninja -C build
	(
		cd build || exit 1
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			ninja install
		else
			as_root ninja install
		fi
	)

	cd "$script_folder" || exit 1
	log_info "Removing downloaded DPDK source directory '$dpdk_folder'."
	rm -rf "$dpdk_folder"
fi
