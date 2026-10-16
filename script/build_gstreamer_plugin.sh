#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] [BUILD_TYPE]

Build the MTL GStreamer plugins of ecosystem/gstreamer_plugin with meson and
ninja, and install them. The build directory is script/build/gstreamer_plugin.
The plugins go into the GStreamer plugin directory of /usr/local by default.
An installed plugin links MTL and DPDK from the install it was built against,
by path, and the script checks that before it finishes.

BUILD_TYPE is one of:
	debug		Debug build, with the test-only packet mutation properties
	release		Release build (default)

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc meson ninja-build pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
	or ./install_dependencies.sh gstreamer
	MTL and DPDK development files must already be installed.

OPTIONS:
	-p, --prefix DIR	Install into DIR/gstreamer-1.0 (default: the libdir of /usr/local)
	-t, --test-tools	Also build and install the test plugins of tests/tools/gstreamer_tools
	-j, --jobs JOBS		Parallel build jobs (default: ${NPROC})
	-h, --help		Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX	Use its parent and install into the gstreamer sibling

EXAMPLES:
	${script_name}
	${script_name} debug --test-tools
	MTL_INSTALL_PREFIX=\$PWD/.local_install/mtl ${script_name}	# into .local_install/gstreamer/gstreamer-1.0
EOF
}

# build_plugins <source dir> <build dir>: meson setup, build and install one
# GStreamer plugin project. Set by main: buildtype, jobs, link_args, prefix_args
# and system_install.
build_plugins() {
	local source_dir=$1 build_dir=$2

	rm -rf "${build_dir}"
	meson setup "${build_dir}" "${source_dir}" --buildtype "${buildtype}" "${prefix_args[@]}" \
		"-Dc_link_args=${link_args}"
	ninja -C "${build_dir}" -j "${jobs}"
	if [ "${system_install}" -eq 1 ]; then
		as_root ninja -C "${build_dir}" install
	else
		ninja -C "${build_dir}" install
	fi
}

main() {
	local buildtype=release jobs="${NPROC}" prefix="" test_tools=0 option
	local local_base="" mtl_pc dpdk_pc plugin_dir libdirs=() link_args plugin
	local -a prefix_args
	local system_install

	while [ "$#" -gt 0 ]; do
		option=$1
		case "${option}" in
		-p | --prefix)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a directory"
				exit 1
			}
			prefix=$2
			shift
			;;
		-t | --test-tools) test_tools=1 ;;
		-j | --jobs)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a job count"
				exit 1
			}
			jobs=$2
			shift
			;;
		-h | --help)
			show_help
			exit 0
			;;
		debug | release) buildtype=${option} ;;
		*)
			log_error "Unknown option: ${option}"
			show_help
			exit 1
			;;
		esac
		shift
	done
	case "${jobs}" in *[!0-9]* | 0 | "")
		log_error "Jobs must be a positive integer: ${jobs}"
		exit 1
		;;
	esac

	if [ -z "${prefix}" ] && [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
		prefix="${local_base}/gstreamer"
	fi

	require_commands "${CC:-cc}:gcc" meson:meson ninja:ninja-build pkg-config:pkg-config realpath:coreutils || exit 1
	if [ -n "${local_base}" ]; then
		mtl_pc=$(find "${local_base}/mtl" -name mtl.pc -type f -print -quit 2>/dev/null || true)
		dpdk_pc=$(find "${local_base}/dpdk" -name libdpdk.pc -type f -print -quit 2>/dev/null || true)
		if [ -z "${mtl_pc}" ] || [ -z "${dpdk_pc}" ]; then
			log_error "MTL and DPDK must be installed beside ${prefix}: ${local_base}/mtl and ${local_base}/dpdk"
			exit 1
		fi
		export PKG_CONFIG_PATH="${mtl_pc%/*}:${dpdk_pc%/*}:${PKG_CONFIG_PATH:-}"
	fi
	require_pkg_config "mtl:MTL (build and install it with build.sh)" "libdpdk:DPDK (build_dpdk.sh)" \
		gstreamer-1.0:libgstreamer1.0-dev gstreamer-base-1.0:libgstreamer-plugins-base1.0-dev \
		gstreamer-video-1.0:libgstreamer-plugins-base1.0-dev gstreamer-audio-1.0:libgstreamer-plugins-base1.0-dev || exit 1

	# A plugin is found by GST_PLUGIN_PATH, so a chosen prefix gets the flat
	# layout DIR/gstreamer-1.0 that the tests and CI point that variable at. The
	# system install goes into the GStreamer plugin directory of the libdir.
	if [ -n "${prefix}" ]; then
		prefix_args=(--prefix "${prefix}" --libdir .)
		plugin_dir="${prefix}/gstreamer-1.0"
		if prefix_writable "${prefix}"; then
			system_install=0
		else
			system_install=1
		fi
	else
		prefix_args=(--prefix /usr/local)
		plugin_dir="/usr/local/$(multiarch_libdir)/gstreamer-1.0"
		system_install=1
	fi
	# The plugins load without ldconfig or LD_LIBRARY_PATH: each records where
	# the shared helper library beside it, MTL and DPDK are, relative to itself
	# (install_rpath in meson.build, from the rpath option). DT_RPATH, not
	# RUNPATH, so that the lookup also covers what libmtl itself needs (DPDK) and
	# so that LD_LIBRARY_PATH cannot point it at another copy.
	libdirs=("$(pkg-config --variable=libdir mtl)" "$(pkg-config --variable=libdir libdpdk)")
	link_args="-Wl,--disable-new-dtags"
	prefix_args+=("-Drpath=$(origin_rpath "${plugin_dir}" "${libdirs[@]}")")

	log_info "Build the GStreamer plugins: buildtype=${buildtype}, into ${plugin_dir}"
	build_plugins "${REPO_DIR}/ecosystem/gstreamer_plugin" "${script_folder}/build/gstreamer_plugin"
	if [ "${test_tools}" -eq 1 ]; then
		log_info "Build the GStreamer test plugins of tests/tools/gstreamer_tools"
		build_plugins "${REPO_DIR}/tests/tools/gstreamer_tools" "${script_folder}/build/gstreamer_tools"
	fi
	[ "${system_install}" -eq 0 ] || ! command_exists ldconfig || as_root ldconfig

	for plugin in "${plugin_dir}"/libgstmtl_*.so; do
		require_resolved_libraries "${plugin}" "${libdirs[@]}" "${plugin_dir}" || exit 1
	done
	if command_exists gst-inspect-1.0; then
		GST_PLUGIN_PATH="${plugin_dir}" GST_REGISTRY="$(mktemp -u)" gst-inspect-1.0 mtl_st20p_tx >/dev/null || {
			log_error "gst-inspect-1.0 cannot load mtl_st20p_tx from ${plugin_dir}"
			exit 1
		}
	fi
	log_success "GStreamer plugins installed in ${plugin_dir}"
	# GStreamer searches its own plugin directory and ~/.local, nothing else.
	if [ "${plugin_dir}" != "$(pkg-config --variable=pluginsdir gstreamer-1.0)" ]; then
		log_info "GStreamer does not search that directory by itself: export GST_PLUGIN_PATH=${plugin_dir}"
	fi
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
