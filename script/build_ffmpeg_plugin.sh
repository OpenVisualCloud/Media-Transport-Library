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

Build FFmpeg with the MTL input and output devices of ecosystem/ffmpeg_plugin,
and install it. The script downloads the FFmpeg release in versions.env into
script/FFmpeg-release-<version>, builds openh264 ${OPENH264_VER} when the system
has none, and installs into /usr/local by default. An installed FFmpeg links
MTL, DPDK and SVT-JPEG-XS from the install it was built against, by path, and
the script checks that before it finishes.

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc make nasm patch pkg-config unzip wget, or ./install_dependencies.sh ffmpeg
	MTL and DPDK development files must already be installed.

OPTIONS:
	-v, --version VERSION	FFmpeg release branch (default: ${FFMPEG_VERSION})
	-p, --prefix DIR	Install into DIR (default: /usr/local)
	-g, --gpu-direct	Compile the MTL devices with GPU direct support
	-j, --jpegxs		Add the SVT-JPEG-XS codecs
	-J, --jobs JOBS		Parallel build jobs (default: ${NPROC})
	-f, --force		Download the source archives again
	-h, --help		Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX	Use its parent and install into the ffmpeg sibling
	FFMPEG_ENABLE_SVT_JPEG_XS	1 is the same as --jpegxs
	SVT_JPEG_XS_REPO	SVT-JPEG-XS sources (default: script/SVT-JPEG-XS)

EXAMPLES:
	${script_name}
	${script_name} --jpegxs --jobs 8
	MTL_INSTALL_PREFIX=\$PWD/.local_install/mtl ${script_name}	# into .local_install/ffmpeg
EOF
}

# The libdir of a pkg-config module, as a plain path: a relocatable .pc file
# gives one with ../ in it.
pkg_libdir() {
	realpath -m "$(pkg-config --variable=libdir "$1")"
}

# Builds openh264 into the prefix, unless the system has a recent one.
build_openh264() {
	local prefix=$1 jobs=$2 source_dir="${script_folder}/openh264-${OPENH264_VER}" archive

	if pkg-config --exists "openh264 >= ${OPENH264_VER}"; then
		log_info "openh264 $(pkg-config --modversion openh264) is installed, not building it"
		return 0
	fi
	archive="${source_dir}.zip"
	[ -f "${archive}" ] || download_file "https://github.com/cisco/openh264/archive/refs/tags/v${OPENH264_VER}.zip" "${archive}"
	rm -rf "${source_dir}"
	unzip -q "${archive}" -d "${script_folder}"
	make -C "${source_dir}" -j "${jobs}" PREFIX="${prefix}"
	if prefix_writable "${prefix}"; then
		make -C "${source_dir}" install PREFIX="${prefix}"
	else
		as_root make -C "${source_dir}" install PREFIX="${prefix}"
	fi
}

# Puts the SVT-JPEG-XS codecs and their FFmpeg patches into the source tree.
add_svt_jpeg_xs() {
	local ffmpeg_dir=$1 repo="${SVT_JPEG_XS_REPO:-${script_folder}/SVT-JPEG-XS}" archive patch_dir patch_file

	if [ ! -f "${repo}/CMakeLists.txt" ]; then
		archive="${repo}.tar.gz"
		rm -rf "${repo}"
		mkdir -p "${repo}"
		download_file "https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/${SVT_JPEG_XS_VER}.tar.gz" "${archive}"
		tar -xzf "${archive}" -C "${repo}" --strip-components=1
		rm -f "${archive}"
	fi
	cp -f "${repo}"/ffmpeg-plugin/libsvtjpegxs* "${ffmpeg_dir}/libavcodec/"
	patch_dir="${repo}/ffmpeg-plugin/${FFMPEG_VERSION}"
	[ -d "${patch_dir}" ] || patch_dir="${repo}/ffmpeg-plugin"
	for patch_file in "${patch_dir}"/*.patch; do
		[ -f "${patch_file}" ] || continue
		log_info "Applying SVT-JPEG-XS patch: $(basename "${patch_file}")"
		patch -d "${ffmpeg_dir}" -p1 --batch --no-backup-if-mismatch -i "${patch_file}"
	done
}

main() {
	local prefix="" jobs="${NPROC}" force=0 gpu_direct=0 jpegxs=0 option
	local local_base="" ffmpeg_dir archive patch_file plugin_dir="${REPO_DIR}/ecosystem/ffmpeg_plugin"
	local mtl_pc dpdk_pc jpeg_pc pc_dirs=() libdirs=() ldflags dir
	local -a configure_flags

	[ "${FFMPEG_ENABLE_SVT_JPEG_XS:-0}" != "1" ] || jpegxs=1
	while [ "$#" -gt 0 ]; do
		option=$1
		case "${option}" in
		-v | --version)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a version"
				exit 1
			}
			FFMPEG_VERSION=$2
			shift
			;;
		-p | --prefix)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a directory"
				exit 1
			}
			prefix=$2
			shift
			;;
		-g | --gpu-direct) gpu_direct=1 ;;
		-j | --jpegxs) jpegxs=1 ;;
		-J | --jobs)
			[ "$#" -ge 2 ] || {
				log_error "${option} needs a job count"
				exit 1
			}
			jobs=$2
			shift
			;;
		-f | --force) force=1 ;;
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
	case "${jobs}" in *[!0-9]* | 0 | "")
		log_error "Jobs must be a positive integer: ${jobs}"
		exit 1
		;;
	esac
	[ -d "${plugin_dir}/${FFMPEG_VERSION}" ] || {
		log_error "No MTL patches for FFmpeg ${FFMPEG_VERSION}: see ${plugin_dir}"
		exit 1
	}

	if [ -z "${prefix}" ]; then
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			prefix="${local_base}/ffmpeg"
		else
			prefix=/usr/local
		fi
	fi

	require_commands "${CC:-cc}:gcc" make:make nasm:nasm patch:patch pkg-config:pkg-config \
		unzip:unzip tar:tar realpath:coreutils || exit 1
	# A local install keeps MTL and DPDK beside the prefix; the system install
	# takes them from the pkg-config path.
	if [ -n "${local_base}" ]; then
		mtl_pc=$(find "${local_base}/mtl" -name mtl.pc -type f -print -quit 2>/dev/null || true)
		dpdk_pc=$(find "${local_base}/dpdk" -name libdpdk.pc -type f -print -quit 2>/dev/null || true)
		if [ -z "${mtl_pc}" ] || [ -z "${dpdk_pc}" ]; then
			log_error "MTL and DPDK must be installed beside ${prefix}: ${local_base}/mtl and ${local_base}/dpdk"
			exit 1
		fi
		pc_dirs+=("${mtl_pc%/*}" "${dpdk_pc%/*}")
		if [ "${jpegxs}" -eq 1 ]; then
			jpeg_pc=$(find "${local_base}/jpegxs" -name SvtJpegxs.pc -type f -print -quit 2>/dev/null || true)
			[ -n "${jpeg_pc}" ] || {
				log_error "--jpegxs needs SVT-JPEG-XS installed in ${local_base}/jpegxs: run build_jpegxs.sh first"
				exit 1
			}
			pc_dirs+=("${jpeg_pc%/*}")
		fi
	fi
	# openh264 lands in the prefix, so its pkg-config file is found there too.
	pc_dirs+=("${prefix}/lib/pkgconfig")
	export PKG_CONFIG_PATH
	PKG_CONFIG_PATH="$(printf '%s:' "${pc_dirs[@]}")${PKG_CONFIG_PATH:-}"
	require_pkg_config "mtl:MTL (build and install it with build.sh)" "libdpdk:DPDK (build_dpdk.sh)" || exit 1
	[ "${jpegxs}" -eq 0 ] || require_pkg_config "SvtJpegxs:SVT-JPEG-XS (build_jpegxs.sh)" || exit 1

	build_openh264 "${prefix}" "${jobs}"

	# The archive stays, the tree is unpacked again: every run patches clean
	# sources, and only --force downloads again.
	ffmpeg_dir="${script_folder}/FFmpeg-release-${FFMPEG_VERSION}"
	archive="${ffmpeg_dir}.zip"
	if [ "${force}" -eq 1 ] || [ ! -f "${archive}" ]; then
		download_file "https://github.com/FFmpeg/FFmpeg/archive/refs/heads/release/${FFMPEG_VERSION}.zip" "${archive}"
	fi
	rm -rf "${ffmpeg_dir}"
	unzip -q "${archive}" -d "${script_folder}"
	cp -f "${plugin_dir}"/mtl_*.c "${plugin_dir}"/mtl_*.h "${ffmpeg_dir}/libavdevice/"
	for patch_file in "${plugin_dir}/${FFMPEG_VERSION}"/*.patch; do
		[ -f "${patch_file}" ] || continue
		log_info "Applying patch: $(basename "${patch_file}")"
		patch -d "${ffmpeg_dir}" -p1 --batch --no-backup-if-mismatch -i "${patch_file}"
	done

	configure_flags=(--prefix="${prefix}" --enable-shared --disable-static --enable-pic
		--enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl)
	[ "${gpu_direct}" -eq 0 ] || configure_flags+=(--extra-cflags=-DMTL_GPU_DIRECT_ENABLED)
	if [ "${jpegxs}" -eq 1 ]; then
		add_svt_jpeg_xs "${ffmpeg_dir}"
		configure_flags+=(--enable-libsvtjpegxs)
	fi
	# The install must run without LD_LIBRARY_PATH: every library it links is
	# recorded by path. DT_RPATH, not RUNPATH, so that the lookup also covers
	# what libmtl itself needs (DPDK) and so that a stale LD_LIBRARY_PATH cannot
	# point it at another copy.
	libdirs=("${prefix}/lib" "$(pkg_libdir mtl)" "$(pkg_libdir libdpdk)")
	[ "${jpegxs}" -eq 0 ] || libdirs+=("$(pkg_libdir SvtJpegxs)")
	ldflags="-L${prefix}/lib -Wl,--disable-new-dtags"
	for dir in "${libdirs[@]}"; do
		ldflags+=" -Wl,-rpath,${dir}"
	done
	configure_flags+=("--extra-ldflags=${ldflags}")

	log_info "Build FFmpeg ${FFMPEG_VERSION} into ${prefix}"
	(cd "${ffmpeg_dir}" && ./configure "${configure_flags[@]}")
	make -C "${ffmpeg_dir}" -j "${jobs}"
	if prefix_writable "${prefix}"; then
		make -C "${ffmpeg_dir}" install
	else
		as_root make -C "${ffmpeg_dir}" install
		case "${prefix}" in
		/usr | /usr/* | /opt | /opt/*) command_exists ldconfig && as_root ldconfig ;;
		esac
	fi

	require_resolved_libraries "${prefix}/bin/ffmpeg" "${libdirs[@]}" || exit 1
	"${prefix}/bin/ffmpeg" -hide_banner -devices 2>/dev/null | grep -q mtl || {
		log_error "${prefix}/bin/ffmpeg has no mtl device"
		exit 1
	}
	log_success "FFmpeg ${FFMPEG_VERSION} with the MTL devices installed in ${prefix}"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
