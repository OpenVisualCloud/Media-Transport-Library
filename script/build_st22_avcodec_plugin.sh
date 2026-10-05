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

Build the ST 2110-22 avcodec plugin in plugins/st22_avcodec with meson and
ninja, and install it. The build directory is script/build/st22_avcodec_plugin.
Install MTL and the FFmpeg development files before you run this script.

BUILD_TYPE is one of:
	debug		Debug build
	plain		Build without extra compiler flags
	release		Release build (default)

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc meson ninja-build pkg-config libavcodec-dev libavutil-dev
	MTL (build and install it with build.sh)

OPTIONS:
	-h		Show this help message

ENVIRONMENT:
	MTL_PLUGIN_PREFIX	Install into this prefix as the current user.
				Without it, the plugin goes into the system as root.

EXAMPLES:
	${script_name}				# Release build, system install
	${script_name} debug			# Debug build
	MTL_PLUGIN_PREFIX=\$PWD/plugins ${script_name}	# Install into a local prefix
EOF
}

check_build_packages() {
	require_commands cc:gcc meson:meson ninja:ninja-build pkg-config:pkg-config || return 1
	require_pkg_config "mtl:MTL (build and install it with build.sh)" \
		libavcodec:libavcodec-dev libavutil:libavutil-dev || return 1
}

main() {
	local opt buildtype=release plugin_build_dir

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

	# .github/actions/build/action.yml uploads the plugin from this directory.
	plugin_build_dir="${script_folder}/build/st22_avcodec_plugin"
	log_info "Build the st22 avcodec plugin: buildtype=${buildtype}"
	# When MTL_PLUGIN_PREFIX is set, install into that prefix (no root, used for the
	# CI .local_install cache). Otherwise fall back to a system-wide install.
	meson setup "${plugin_build_dir}" "${REPO_DIR}/plugins/st22_avcodec" -Dbuildtype="$buildtype" \
		${MTL_PLUGIN_PREFIX:+"--prefix=${MTL_PLUGIN_PREFIX}"}
	ninja -C "${plugin_build_dir}"
	if [ -n "${MTL_PLUGIN_PREFIX:-}" ]; then
		ninja -C "${plugin_build_dir}" install
	else
		as_root ninja -C "${plugin_build_dir}" install
	fi
	log_success "st22 avcodec plugin installed"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
