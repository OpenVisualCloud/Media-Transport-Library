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
updates kahawai.json to enable the installed plugin.

REQUIRED PACKAGES:
	C/C++ compiler, cmake, meson, ninja, pkg-config, python3, tar, curl or wget
	MTL and DPDK development files must already be installed.

OPTIONS:
	-f, --force		Refresh the managed source download and rebuild
	-p, --prefix DIR	Install into DIR (default: /usr/local)
	-s, --source-dir DIR	Use or download sources in DIR
	-v, --version VERSION	Build VERSION (default: ${SVT_JPEG_XS_VER})
	-j, --jobs JOBS		Parallel build jobs (default: ${NPROC})
	    --config FILE	Update this plugin registry (default: kahawai.json)
	    --ci		Build and validate a relocatable CI bundle
	-h, --help		Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX	Use its parent and install into the jpegxs sibling
	JPEGXS_ROOT		Install prefix; overridden by --prefix
	KAHAWAI_CFG_PATH	Plugin registry; overridden by --config
	CC, CXX		Compiler selection

EXAMPLES:
	${script_name}
	${script_name} --prefix "\$HOME/.local" --jobs 8
	${script_name} --source-dir /src/SVT-JPEG-XS --version main
EOF
}

download_source() {
	local archive=$1 url=$2

	if command_exists curl; then
		curl --fail --location --retry 3 --connect-timeout 15 --max-time 180 \
			"${url}" --output "${archive}"
	elif command_exists wget; then
		wget --tries=3 --timeout=180 "${url}" -O "${archive}"
	else
		log_error "Required package is missing: curl or wget"
		return 1
	fi
}

compiler_sha256() {
	{
		"${CC:-cc}" -dumpmachine
		"${CC:-cc}" -dumpfullversion -dumpversion
	} | python3 -c 'import hashlib, sys; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())'
}

write_bundle_metadata() {
	local stage=$1 compiler_hash=$2 source_hash=$3

	cat >"${stage}/bundle.env" <<EOF
schema=1
svt_jpeg_xs_revision=${SVT_JPEG_XS_VER}
architecture=$(uname -m)
compiler_sha256=${compiler_hash}
source_hash=${source_hash}
EOF
	python3 - "${stage}" <<'PY'
import hashlib
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
files = []
links = []
for path in root.rglob("*"):
    relative = "./" + path.relative_to(root).as_posix()
    if path.is_symlink():
        links.append(f"{relative}={os.readlink(path)}")
    elif path.is_file() and path.name not in {"manifest.sha256", "symlinks.manifest"}:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        files.append(f"{digest}  {relative}")
(root / "symlinks.manifest").write_text("\n".join(sorted(links)) + ("\n" if links else ""))
(root / "manifest.sha256").write_text("\n".join(sorted(files)) + "\n")
PY
}

register_plugin() {
	local config=$1 plugin=$2

	python3 - "${config}" "${plugin}" <<'PY'
import json
import os
import pathlib
import stat
import sys
import tempfile

config = pathlib.Path(sys.argv[1])
plugin_path = str(pathlib.Path(sys.argv[2]).resolve())
config_mode = stat.S_IMODE(config.stat().st_mode)
data = json.loads(config.read_text())
plugins = data.setdefault("plugins", [])
matches = [index for index, item in enumerate(plugins) if item.get("name") == "st22_svt_jpegxs"]
if matches:
    selected = next((index for index in matches if plugins[index].get("path") == plugin_path), matches[0])
    for index in matches:
        plugins[index]["enabled"] = int(index == selected)
    plugins[selected]["path"] = plugin_path
else:
    plugins.append({"enabled": 1, "name": "st22_svt_jpegxs", "path": plugin_path})
fd, temporary = tempfile.mkstemp(prefix=config.name + ".", dir=config.parent)
os.chmod(temporary, config_mode)
try:
    with os.fdopen(fd, "w") as output:
        json.dump(data, output, indent=4)
        output.write("\n")
    os.replace(temporary, config)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
PY
	log_success "Enabled JPEG XS plugin in ${config}: ${plugin}"
}

main() {
	local prefix="${JPEGXS_ROOT:-}" source_dir="" config="${KAHAWAI_CFG_PATH:-${REPO_DIR}/kahawai.json}"
	local jobs="${NPROC}" force=0 ci=0 source_dir_explicit=0 fetch_source=0
	local option archive url stage jpeg_pc plugin plugin_relative
	local local_base="" mtl_pc="" dpdk_pc="" compiler_hash expected_compiler_hash source_hash
	local pinned_version="${SVT_JPEG_XS_VER}"

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
		--ci) ci=1 ;;
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

	if [ -z "${prefix}" ]; then
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			prefix="${local_base}/jpegxs"
		else
			prefix=/usr/local
		fi
	fi
	[ -n "${source_dir}" ] || source_dir="${script_folder}/SVT-JPEG-XS"
	if [ "${ci}" -eq 1 ] && [ "${SVT_JPEG_XS_VER}" != "${pinned_version}" ]; then
		log_error "--version cannot be used with --ci; CI bundles use the revision in versions.env"
		exit 1
	fi
	if [ "${source_dir_explicit}" -eq 1 ] && [ -e "${source_dir}" ] && [ ! -f "${source_dir}/CMakeLists.txt" ]; then
		log_error "Source directory does not contain CMakeLists.txt: ${source_dir}"
		exit 1
	fi

	require_commands "${CC:-cc}:gcc" cmake:cmake meson:meson ninja:ninja-build \
		pkg-config:pkg-config python3:python3 tar:tar find:find sed:sed || exit 1
	[ -f "${config}" ] || {
		log_error "Plugin registry does not exist: ${config}"
		exit 1
	}
	case "${jobs}" in *[!0-9]* | 0 | "")
		log_error "Jobs must be a positive integer: ${jobs}"
		exit 1
		;;
	esac
	if [ "${ci}" -eq 1 ]; then
		compiler_hash=$(compiler_sha256)
		expected_compiler_hash=$(bash "${REPO_DIR}/.github/scripts/ci/compiler-identity.sh" producer)
		if [ "${compiler_hash}" != "${expected_compiler_hash}" ]; then
			log_error "JPEG XS producer compiler does not match the CI toolchain contract"
			exit 1
		fi
	fi

	if [ "${ci}" -eq 1 ] && [ "${force}" -eq 0 ] &&
		JPEGXS_ROOT="${prefix}" bash "${REPO_DIR}/.github/scripts/ci/validate-jpegxs.sh" >/dev/null 2>&1; then
		plugin=$(find "${prefix}" -name libst_plugin_st22_svt_jpeg_xs.so -type f -print -quit)
		register_plugin "${config}" "${plugin}"
		log_success "JPEG XS bundle is already valid: ${prefix}"
		exit 0
	fi

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
		download_source "${archive}" "${url}"
		tar -xzf "${archive}" -C "${source_dir}" --strip-components=1
		rm -f "${archive}"
		echo "${SVT_JPEG_XS_VER}" >"${source_dir}/.mtl-revision"
	fi

	stage=$(mktemp -d "${TMPDIR:-/tmp}/mtl-jpegxs.XXXXXX")
	trap 'rm -rf "${stage}"' EXIT
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
	if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
		[ -n "${local_base}" ] || local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
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

	meson setup "${source_dir}/imtl-plugin/build-mtl" "${source_dir}/imtl-plugin" \
		--buildtype release --prefix "${stage}"
	meson compile -C "${source_dir}/imtl-plugin/build-mtl" -j "${jobs}"
	meson install -C "${source_dir}/imtl-plugin/build-mtl"
	python3 - "${jpeg_pc}" "${stage}" <<'PY'
import os
import pathlib
import sys

pc_file = pathlib.Path(sys.argv[1])
stage = pathlib.Path(sys.argv[2])
relative = os.path.relpath(stage, pc_file.parent)
lines = pc_file.read_text().splitlines()
lines = [f"prefix=${{pcfiledir}}/{relative}" if line.startswith("prefix=") else line for line in lines]
pc_file.write_text("\n".join(lines) + "\n")
PY

	plugin=$(find "${stage}" -name libst_plugin_st22_svt_jpeg_xs.so -type f -print -quit)
	[ -n "${plugin}" ] || {
		log_error "MTL JPEG XS bridge plugin was not installed"
		exit 1
	}
	plugin_relative=${plugin#"${stage}/"}
	if [ "${ci}" -eq 1 ]; then
		source_hash=$(bash "${REPO_DIR}/script/hash_sources.sh" -o /dev/stdout | sed -n 's/^jpegxs=//p')
		write_bundle_metadata "${stage}" "${compiler_hash}" "${source_hash}"
	fi

	if [ "${ci}" -eq 1 ]; then
		JPEGXS_ROOT="${stage}" bash "${REPO_DIR}/.github/scripts/ci/validate-jpegxs.sh"
		mkdir -p "$(dirname "${prefix}")"
		rm -rf "${prefix}"
		mv "${stage}" "${prefix}"
		trap - EXIT
	elif mkdir -p "${prefix}" 2>/dev/null; then
		cp -a "${stage}/." "${prefix}/"
	else
		as_root mkdir -p "${prefix}"
		as_root cp -R "${stage}/." "${prefix}/"
	fi
	case "${prefix}" in
	/usr | /usr/* | /opt | /opt/*) command_exists ldconfig && as_root ldconfig ;;
	esac

	register_plugin "${config}" "${prefix}/${plugin_relative}"
	log_success "JPEG XS ${SVT_JPEG_XS_VER} installed in ${prefix}"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
