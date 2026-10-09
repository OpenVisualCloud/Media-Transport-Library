#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] [BUILD_TYPE]

Build the MTL sample applications in app/ with meson and ninja. The build
directory is build/app in the repository root. Install MTL before you run
this script.

BUILD_TYPE is one of:
	debug		Debug build with AddressSanitizer
	debugonly	Debug build without AddressSanitizer
	debugoptimized	Debug build with optimization
	plain		Build without extra compiler flags
	release		Release build (default)

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc meson ninja-build pkg-config libjson-c-dev libpcap-dev
	MTL (build and install it with build.sh)

OPTIONS:
	-h		Show this help message

ENVIRONMENT:
	MTL_BUILD_ENABLE_ASAN	Set to "true" to enable AddressSanitizer.
				The default build type then is debug.

EXAMPLES:
	${script_name}			# Release build
	${script_name} debugonly	# Debug build without AddressSanitizer
EOF
}

check_build_packages() {
	require_commands cc:gcc meson:meson ninja:ninja-build pkg-config:pkg-config || return 1
	require_pkg_config "mtl:MTL (build and install it with build.sh)" \
		json-c:libjson-c-dev libpcap:libpcap-dev || return 1
}

main() {
	local opt buildtype=release enable_asan=false app_build_dir

	if [ "${MTL_BUILD_ENABLE_ASAN:-}" == "true" ]; then
		enable_asan=true
		buildtype=debug # use debug build as default for asan
		log_info "Enable asan check."
	fi

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
	debug)
		buildtype=debug
		enable_asan=true
		;;
	debugonly) buildtype=debug ;;
	debugoptimized) buildtype=debugoptimized ;;
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

	app_build_dir="${REPO_DIR}/build/app"
	log_info "Build the sample applications: buildtype=${buildtype}, asan=${enable_asan}"
	meson setup "${app_build_dir}" "${REPO_DIR}/app" -Dbuildtype="$buildtype" -Denable_asan="$enable_asan"
	ninja -C "${app_build_dir}"
	log_success "Sample applications built in ${app_build_dir}"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
