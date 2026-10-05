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
Usage: ${script_name} [OPTIONS] [BUILD_TYPE]

Build the OBS Studio MTL plugin in ecosystem/obs_mtl/linux-mtl with meson and
ninja, and install it into the system. The build directory is build/obs_plugin
in the repository root. Install MTL and the OBS Studio development files
before you run this script.

BUILD_TYPE is one of:
	debug		Debug build
	plain		Build without extra compiler flags
	release		Release build (default)

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc meson ninja-build pkg-config libobs-dev
	MTL (build and install it with build.sh)

OPTIONS:
	-h		Show this help message

EXAMPLES:
	${script_name}		# Release build
	${script_name} debug	# Debug build
EOF
}

check_build_packages() {
	require_commands cc:gcc meson:meson ninja:ninja-build pkg-config:pkg-config || return 1
	require_pkg_config "mtl:MTL (build and install it with build.sh)" \
		libobs:libobs-dev || return 1
}

main() {
	local opt buildtype=release obs_plugin_build_dir

	while getopts ":h" opt; do
		case $opt in
		h)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown option: -${OPTARG}"
			show_help
			exit 1
			;;
		esac
	done
	shift $((OPTIND - 1))

	case "${1-}" in
	"") ;;
	debug) buildtype=debug ;;
	plain) buildtype=plain ;;
	release) buildtype=release ;;
	*)
		log_error "Unknown build type: $1"
		show_help
		exit 1
		;;
	esac
	[ "$#" -le 1 ] || {
		log_error "Unexpected argument: $2"
		show_help
		exit 1
	}

	check_build_packages || exit 1

	obs_plugin_build_dir="${REPO_DIR}/build/obs_plugin"
	log_info "Build the OBS plugin: buildtype=${buildtype}"
	meson setup "${obs_plugin_build_dir}" "${REPO_DIR}/ecosystem/obs_mtl/linux-mtl" -Dbuildtype="$buildtype"
	ninja -C "${obs_plugin_build_dir}"
	as_root ninja -C "${obs_plugin_build_dir}" install
	log_success "OBS plugin installed"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
