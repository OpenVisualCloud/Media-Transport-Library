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
Usage: ${script_name} [OPTIONS]

Build and install SVT-JPEG-XS and its MTL ST 2110-22 bridge plugin. The script
uses the version in versions.env, installs into /usr/local by default, and
enables the installed plugin in the plugin registry (kahawai.json).

REQUIRED PACKAGES:
	C/C++ compiler, cmake, meson, ninja, pkg-config, python3, tar, curl or wget
	MTL and DPDK development files must already be installed.

OPTIONS:
	-f, --force		Refresh the managed source download and rebuild
	-p, --prefix DIR	Install into DIR (default: /usr/local)
	-s, --source-dir DIR	Use or download sources in DIR
	-v, --version VERSION	Build VERSION (default: ${SVT_JPEG_XS_VER})
	-j, --jobs JOBS		Parallel build jobs (default: ${NPROC})
	    --config FILE	Update this plugin registry (default: conf/kahawai.json
				beside the local prefix, else kahawai.json)
	-h, --help		Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX	Take MTL and DPDK from its parent, and install into the
				jpegxs sibling unless --prefix says otherwise
	JPEGXS_ROOT		Install prefix; overridden by --prefix
	KAHAWAI_CFG_PATH	Plugin registry; overridden by --config
	CC, CXX		Compiler selection

EXAMPLES:
	${script_name}
	${script_name} --prefix "\$HOME/.local" --jobs 8
	${script_name} --source-dir /src/SVT-JPEG-XS --version main
EOF
}

main() {
	local prefix="${JPEGXS_ROOT:-}" source_dir="" config="${KAHAWAI_CFG_PATH:-}"
	local jobs="${NPROC}" force=0 source_dir_explicit=0 fetch_source=0
	local option archive url stage cleanup jpeg_pc plugin plugin_relative plugin_libdir
	local local_base="" mtl_pc="" dpdk_pc=""

	while [ "$#" -gt 0 ]; do
		option=$1
		case "${option}" in
		-f | --force) force=1 ;;
		-p | --prefix)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a directory"
				exit 1
			}
			prefix=$2
			shift
			;;
		-s | --source-dir)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a directory"
				exit 1
			}
			source_dir=$2
			source_dir_explicit=1
			shift
			;;
		-v | --version)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a version"
				exit 1
			}
			SVT_JPEG_XS_VER=$2
			shift
			;;
		-j | --jobs)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a job count"
				exit 1
			}
			jobs=$2
			shift
			;;
		--config)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a file"
				exit 1
			}
			config=$2
			shift
			;;
		-h | --help)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown option: ${option}"
			show_help
			exit 1
			;;
		esac
		shift
	done

	# MTL_INSTALL_PREFIX says where MTL and DPDK are; --prefix says where JPEG XS
	# goes. Without --prefix it goes beside them.
	[ -z "${MTL_INSTALL_PREFIX:-}" ] || local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
	# build.sh writes the registry of a local install beside it.
	[ -n "${config}" ] || config="${local_base:+${local_base}/conf/kahawai.json}"
	config="${config:-${REPO_DIR}/kahawai.json}"
	if [ -z "${prefix}" ]; then
		prefix="${local_base:+${local_base}/jpegxs}"
		prefix="${prefix:-/usr/local}"
	fi
	[ -n "${source_dir}" ] || source_dir="${script_folder}/SVT-JPEG-XS"
	if [ "${source_dir_explicit}" -eq 1 ] && [ -e "${source_dir}" ] && [ ! -f "${source_dir}/CMakeLists.txt" ]; then
		log_error "Source directory does not contain CMakeLists.txt: ${source_dir}"
		exit 1
	fi

	require_commands "${CC:-cc}:gcc" cmake:cmake meson:meson ninja:ninja-build \
		pkg-config:pkg-config python3:python3 tar:tar find:find sed:sed realpath:coreutils || exit 1
	[ -f "${config}" ] || {
		log_error "Plugin registry does not exist: ${config}"
		exit 1
	}
	case "${jobs}" in *[!0-9]* | 0 | "")
		log_error "Jobs must be a positive integer: ${jobs}"
		exit 1
		;;
	esac

	if [ "${source_dir_explicit}" -eq 1 ]; then
		[ -f "${source_dir}/CMakeLists.txt" ] || fetch_source=1
	elif [ "${force}" -eq 1 ] || [ ! -f "${source_dir}/CMakeLists.txt" ] ||
		[ "$(cat "${source_dir}/.mtl-revision" 2>/dev/null || true)" != "${SVT_JPEG_XS_VER}" ]; then
		fetch_source=1
	fi
	if [ "${fetch_source}" -eq 1 ]; then
		rm -rf "${source_dir}"
		mkdir -p "${source_dir}"
		archive="${source_dir}.tar.gz"
		url="https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/${SVT_JPEG_XS_VER}.tar.gz"
		download_file "${url}" "${archive}"
		tar -xzf "${archive}" -C "${source_dir}" --strip-components=1
		rm -f "${archive}"
		echo "${SVT_JPEG_XS_VER}" >"${source_dir}/.mtl-revision"
	fi

	stage=$(mktemp -d "${TMPDIR:-/tmp}/mtl-jpegxs.XXXXXX")
	# Expand stage now: it is local to main, and the trap runs after main returns.
	printf -v cleanup 'rm -rf %q' "${stage}"
	# shellcheck disable=SC2064
	trap "${cleanup}" EXIT
	rm -rf "${source_dir}/Build/mtl" "${source_dir}/imtl-plugin/build-mtl"
	cmake -S "${source_dir}" -B "${source_dir}/Build/mtl" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="${stage}" -DBUILD_SHARED_LIBS=ON
	cmake --build "${source_dir}/Build/mtl" --parallel "${jobs}"
	cmake --install "${source_dir}/Build/mtl"

	jpeg_pc=$(find "${stage}" -name SvtJpegxs.pc -type f -print -quit)
	[ -n "${jpeg_pc}" ] || {
		log_error "SVT-JPEG-XS did not install SvtJpegxs.pc"
		exit 1
	}
	if [ -n "${local_base}" ]; then
		mtl_pc=$(find "${local_base}/mtl" -name mtl.pc -type f -print -quit)
		dpdk_pc=$(find "${local_base}/dpdk" -name libdpdk.pc -type f -print -quit)
		if [ -z "${mtl_pc}" ] || [ -z "${dpdk_pc}" ]; then
			log_error "MTL and DPDK pkg-config files must be installed beside ${prefix}"
			exit 1
		fi
		export PKG_CONFIG_PATH="${jpeg_pc%/*}:${mtl_pc%/*}:${dpdk_pc%/*}:${PKG_CONFIG_PATH:-}"
	else
		export PKG_CONFIG_PATH="${jpeg_pc%/*}:${PKG_CONFIG_PATH:-}"
		require_pkg_config mtl:mtl libdpdk:dpdk || exit 1
	fi

	# The plugin lands in <prefix>/lib/<multiarch>; libSvtJpegxs in <prefix>/lib.
	# Both, MTL and DPDK are recorded relative to the plugin, for the install's
	# final place.
	plugin_libdir="${prefix}/$(multiarch_libdir)"
	meson setup "${source_dir}/imtl-plugin/build-mtl" "${source_dir}/imtl-plugin" \
		--buildtype release --prefix "${stage}" \
		"-Dc_link_args=$(origin_rpath_args "${plugin_libdir}" "${prefix}/lib" \
			"$(pkg-config --variable=libdir mtl)" "$(pkg-config --variable=libdir libdpdk)")"
	meson compile -C "${source_dir}/imtl-plugin/build-mtl" -j "${jobs}"
	meson install -C "${source_dir}/imtl-plugin/build-mtl"
	relocate_pkgconfig "${stage}"

	plugin=$(find "${stage}" -name libst_plugin_st22_svt_jpeg_xs.so -type f -print -quit)
	[ -n "${plugin}" ] || {
		log_error "MTL JPEG XS bridge plugin was not installed"
		exit 1
	}
	plugin_relative=${plugin#"${stage}/"}

	if prefix_writable "${prefix}"; then
		cp -a "${stage}/." "${prefix}/"
	else
		as_root mkdir -p "${prefix}"
		as_root cp -R "${stage}/." "${prefix}/"
	fi
	case "${prefix}" in
	/usr | /usr/* | /opt | /opt/*) command_exists ldconfig && as_root ldconfig ;;
	esac

	require_resolved_libraries "${prefix}/${plugin_relative}" "${prefix}/lib" \
		"$(pkg-config --variable=libdir mtl)" "$(pkg-config --variable=libdir libdpdk)" || exit 1
	register_plugin "${config}" st22_svt_jpegxs "${prefix}/${plugin_relative}"
	log_success "JPEG XS ${SVT_JPEG_XS_VER} installed in ${prefix}"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
