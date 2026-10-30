#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS]

Build and install DPDK with MTL patches.

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc meson ninja-build patch pkg-config python3 python3-pyelftools
	libnuma-dev unzip wget

OPTIONS:
	-f		Force rebuild (reuses existing DPDK source if present)
	-v VERSION	Specify DPDK version to build (default: ${DPDK_VER})
	-h, --help	Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX	The MTL prefix: install into the dpdk sibling of its parent
				(.local_install/dpdk beside .local_install/mtl) instead of the system
	DPDK_MAX_LCORES		RTE_MAX_LCORE, the highest CPU ID + 1 MTL can use (default: the
				CPU count of this host, at least 256; at most 1024, as MTL)

EXAMPLES:
	${script_name}			# Build the default DPDK version
	${script_name} -v 25.11		# Build DPDK 25.11
	${script_name} -f -v 25.11	# Force rebuild of DPDK 25.11
EOF
}

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

# Sets DPDK_PYTHON to a python3 that can import elftools. meson finds python3
# in PATH, so the build puts the directory of that python3 first.
check_build_packages() {
	local candidate missing=0

	require_commands cc:gcc meson:meson ninja:ninja-build patch:patch \
		pkg-config:pkg-config python3:python3 unzip:unzip wget:wget || missing=1
	DPDK_PYTHON=""
	for candidate in "$(command -v python3 2>/dev/null || true)" /usr/bin/python3 /usr/local/bin/python3; do
		if [ -x "$candidate" ] && "$candidate" -c 'import elftools' 2>/dev/null; then
			DPDK_PYTHON="$candidate"
			break
		fi
	done
	if command_exists python3 && [ -z "$DPDK_PYTHON" ]; then
		log_error "Required package is missing: ${PYELFTOOLS_PACKAGE:-Python elftools module}"
		missing=1
	fi
	if command_exists pkg-config && ! pkg-config --exists numa; then
		log_error "Required package is missing: ${NUMA_DEVEL_PACKAGE:-NUMA development files}"
		missing=1
	fi

	[ "$missing" -eq 0 ] && return 0
	if [ "$FORCE" -eq 1 ]; then
		log_warning "Cannot find all required packages. Continuing because force mode is enabled."
		return 0
	fi
	log_error "Cannot build DPDK. Install the missing packages, or use -f."
	return 1
}

main() {
	local opt dpdk_folder archive_name patch_file python_path="$PATH" dpdk_prefix max_lcores

	FORCE=0
	while getopts "fhv:-:" opt; do
		case $opt in
		f) FORCE=1 ;;
		v) DPDK_VER="$OPTARG" ;;
		h | -)
			if [ "${opt}" = - ] && [ "${OPTARG}" != help ]; then
				log_error "Unknown option: --${OPTARG}"
				show_help
				exit 1
			fi
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
	[ "$#" -eq 0 ] || {
		log_error "Unexpected argument: $1"
		show_help
		exit 1
	}

	cd "${script_folder}"
	dpdk_folder="dpdk-${DPDK_VER}"
	log_info "Attempting to install DPDK version: ${DPDK_VER}.${DPDK_MTL_MINOR_VER}"
	# The install scripts of DPDK split a path at a space.
	if [ -n "${MTL_INSTALL_PREFIX:-}" ] && [[ "$(dirname "${MTL_INSTALL_PREFIX}")" == *" "* ]]; then
		log_error "DPDK cannot install into a path with a space: $(dirname "${MTL_INSTALL_PREFIX}")/dpdk"
		exit 1
	fi

	# Skip rebuild if the correct version is already installed system-wide.
	# Local-prefix installs always rebuild. Use -f to force.
	if [ "$FORCE" -eq 0 ] && [ -z "${MTL_INSTALL_PREFIX:-}" ] && dpdk_is_installed; then
		log_success "DPDK already installed ($(pkg-config --modversion libdpdk)). Skipping rebuild."
		exit 0
	fi

	# A successful run removes the source directory, so this one is from a run that stopped.
	if [ "$FORCE" -eq 0 ] && [ -d "$dpdk_folder" ]; then
		log_error "'$dpdk_folder' is from an earlier run that did not complete. Remove it, or use -f to build from it."
		exit 1
	fi

	check_build_packages || exit 1
	# The stamp file shows that the unzip and the patches are complete.
	if [ -f "$dpdk_folder/.mtl-patched" ]; then
		log_warning "Force rebuild enabled. Reusing existing '$dpdk_folder' directory."
	elif [ -d "$dpdk_folder" ]; then
		log_warning "Removing '$dpdk_folder': an earlier run stopped before the patches were complete."
		rm -rf "$dpdk_folder"
	fi

	if [ ! -d "$dpdk_folder" ]; then
		log_info "Clone DPDK source code"
		archive_name="v${DPDK_VER}.zip"
		rm -f "$archive_name"
		wget "https://github.com/DPDK/dpdk/archive/refs/tags/${archive_name}" -O "$archive_name" || {
			rm -f "$archive_name"
			log_error "Cannot download ${archive_name}."
			exit 1
		}
		unzip -q "$archive_name"
		rm -f "$archive_name"

		cd "$dpdk_folder"
		for patch_file in "${REPO_DIR}/patches/dpdk/${DPDK_VER}"/*.patch; do
			patch -p1 -i "$patch_file"
		done
		touch .mtl-patched
	else
		cd "$dpdk_folder"
	fi

	# An MTL lcore id is the CPU id, so DPDK must number every CPU of the host.
	max_lcores=$(nproc --all)
	# An image built on a small host still runs on 256 CPUs: ~1 MiB more per mempool.
	[ "$max_lcores" -lt 256 ] && max_lcores=256
	max_lcores=${DPDK_MAX_LCORES:-$max_lcores}
	if [ "$max_lcores" -gt 1024 ]; then
		log_warning "max_lcores ${max_lcores} is above 1024, the most CPUs MTL supports. Using 1024."
		max_lcores=1024
	fi
	log_info "Build and install DPDK now, max_lcores ${max_lcores}"
	[ -n "${DPDK_PYTHON}" ] && python_path="$(dirname "$DPDK_PYTHON"):$PATH"
	if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		# A prefix install is self-contained and can be moved: each library
		# records its own directory, and each driver in dpdk/pmds-<ABI> the
		# library directory two levels up, relative to itself ($ORIGIN, as
		# DT_RPATH), and each .pc file names its own location. The
		# linker follows those rpaths too, so a consumer of libmtl resolves
		# DPDK's dependencies without LD_LIBRARY_PATH.
		dpdk_prefix="$(dirname "${MTL_INSTALL_PREFIX}")/dpdk"
		# shellcheck disable=SC2016 # $ORIGIN is for the loader
		PATH="$python_path" meson setup build --prefix "${dpdk_prefix}" -Dmax_lcores="${max_lcores}" \
			'-Dc_link_args=-Wl,--disable-new-dtags,-rpath,$ORIGIN,-rpath,$ORIGIN/../..'
		ninja -C build
		ninja -C build install
		relocate_pkgconfig "${dpdk_prefix}"
	else
		PATH="$python_path" meson setup build -Dmax_lcores="${max_lcores}"
		ninja -C build
		as_root ninja -C build install
		command_exists ldconfig && as_root ldconfig
	fi

	cd "${script_folder}"
	log_info "Removing downloaded DPDK source directory '$dpdk_folder'."
	rm -rf "$dpdk_folder"
	log_success "DPDK ${DPDK_VER} installed"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
