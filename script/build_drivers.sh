#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
# shellcheck disable=SC1091
. "${REPO_DIR}/versions.env"

DRIVER="ice"
BUILD_ONLY=false
FORCE=false
if [[ "${FORCE_ICE_REBUILD:-0}" == "1" ]]; then
	FORCE=true
fi
DPDK_SRC_DIR=""
BUILDTYPE="release"
SKIP_DPDK=false

usage() {
	cat <<USAGE
Usage: $(basename "$0") [OPTIONS]

Build drivers used by Media Transport Library.

Options:
  --driver <ice|igc>         Driver flow to build (default: ice)
  --build-only              Compile ICE without installing or loading it
  --ice-version <version>    ICE version (default: ${ICE_VER})
  --ice-download-id <id>     Intel download mirror ID (default: ${ICE_DMID})
  --dpdk-ver <version>       DPDK version for IGC (default: ${DPDK_VER})
  --dpdk-src-dir <path>      DPDK source directory (default: ../dpdk-<version>)
  --buildtype <type>         MTL build type for IGC (default: ${BUILDTYPE})
  --force                    Rebuild ICE or re-clone DPDK
  --skip-dpdk                Build only MTL in the IGC flow
  -h, --help                 Show this help
USAGE
}

run_as_root() {
	if [[ "${EUID}" -eq 0 ]]; then
		"$@"
	elif command -v sudo >/dev/null 2>&1; then
		sudo "$@"
	else
		echo "Need root privileges to run: $*" >&2
		exit 1
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--driver)
		[[ $# -ge 2 ]] || { echo "--driver requires a value" >&2; exit 1; }
		DRIVER="$2"
		shift 2
		;;
	--build-only)
		BUILD_ONLY=true
		shift
		;;
	--ice-version)
		[[ $# -ge 2 ]] || { echo "--ice-version requires a value" >&2; exit 1; }
		ICE_VER="$2"
		shift 2
		;;
	--ice-download-id)
		[[ $# -ge 2 ]] || { echo "--ice-download-id requires a value" >&2; exit 1; }
		ICE_DMID="$2"
		shift 2
		;;
	--dpdk-ver)
		[[ $# -ge 2 ]] || { echo "--dpdk-ver requires a value" >&2; exit 1; }
		DPDK_VER="$2"
		shift 2
		;;
	--dpdk-src-dir)
		[[ $# -ge 2 ]] || { echo "--dpdk-src-dir requires a value" >&2; exit 1; }
		DPDK_SRC_DIR="$2"
		shift 2
		;;
	--buildtype)
		[[ $# -ge 2 ]] || { echo "--buildtype requires a value" >&2; exit 1; }
		BUILDTYPE="$2"
		shift 2
		;;
	--force)
		FORCE=true
		shift
		;;
	--skip-dpdk)
		SKIP_DPDK=true
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		usage
		exit 1
		;;
	esac
done

build_ice() {
	local archive_name="ice-${ICE_VER}.tar.gz"
	local github_archive=0

	if [[ "${BUILD_ONLY}" == "false" && "${FORCE}" == "false" ]] &&
		sudo modinfo ice 2>/dev/null | grep -Ei "^version:[[:space:]]*Kahawai_${ICE_VER}" >/dev/null; then
		echo "ICE driver version ${ICE_VER} (Kahawai) is already installed. Skipping rebuild."
		return
	fi

	cd "${SCRIPT_DIR}"
	if [[ -f "${archive_name}" ]] && gzip -t "${archive_name}" >/dev/null 2>&1; then
		echo "Found valid local archive ${archive_name}, skipping download."
		if tar -tzf "${archive_name}" | grep "^ethernet-linux-ice" >/dev/null; then
			github_archive=1
		fi
	else
		rm -f "${archive_name}"
		wget "https://downloadmirror.intel.com/${ICE_DMID}/${archive_name}" -O "${archive_name}" || true
		if [[ ! -f "${archive_name}" ]] || ! gzip -t "${archive_name}" >/dev/null 2>&1; then
			rm -f "${archive_name}"
			wget "https://github.com/intel/ethernet-linux-ice/archive/refs/tags/v${ICE_VER}.tar.gz" -O "${archive_name}" || true
			if [[ -f "${archive_name}" ]] && gzip -t "${archive_name}" >/dev/null 2>&1; then
				github_archive=1
			else
				echo "Failed to download a valid ${archive_name}." >&2
				rm -f "${archive_name}"
				exit 1
			fi
		fi
	fi

	if [[ -d "ice-${ICE_VER}" ]]; then
		if [[ "${FORCE}" == "true" ]]; then
			rm -rf "ice-${ICE_VER}"
		else
			echo "ice-${ICE_VER} already exists. Use --force to replace it." >&2
			exit 1
		fi
	fi

	tar xzf "${archive_name}"
	rm -f "${archive_name}"
	if [[ "${github_archive}" -eq 1 && -d "ethernet-linux-ice-${ICE_VER}" ]]; then
		mv "ethernet-linux-ice-${ICE_VER}" "ice-${ICE_VER}"
	fi
	[[ -d "ice-${ICE_VER}" ]] || { echo "Failed to extract ${archive_name}." >&2; exit 1; }

	pushd "ice-${ICE_VER}" >/dev/null
	for patch_file in "${REPO_DIR}"/patches/ice_drv/"${ICE_VER}"/*.patch; do
		patch -p1 -i "${patch_file}"
	done
	make -C src -j"$(nproc)"
	if [[ "${BUILD_ONLY}" == "false" ]]; then
		run_as_root make -C src install
		run_as_root rmmod irdma || true
		run_as_root rmmod ice
		run_as_root modprobe ice
	fi
	popd >/dev/null
	rm -rf "ice-${ICE_VER}"
}

build_igc() {
	case "${BUILDTYPE}" in
	debug | debugonly | debugoptimized | plain | release) ;;
	*) echo "Invalid --buildtype '${BUILDTYPE}'" >&2; exit 1 ;;
	esac

	if [[ -z "${DPDK_SRC_DIR}" ]]; then
		DPDK_SRC_DIR="${REPO_DIR}/../dpdk-${DPDK_VER}"
	fi

	if [[ "${SKIP_DPDK}" == "false" ]]; then
		if [[ "${FORCE}" == "true" ]]; then
			rm -rf "${DPDK_SRC_DIR}"
		fi
		if [[ ! -d "${DPDK_SRC_DIR}" ]]; then
			git -C "$(dirname "${DPDK_SRC_DIR}")" clone https://github.com/DPDK/dpdk.git "$(basename "${DPDK_SRC_DIR}")"
		fi
		[[ -d "${DPDK_SRC_DIR}/.git" ]] || { echo "Not a git repository: ${DPDK_SRC_DIR}" >&2; exit 1; }
		[[ -z "$(git -C "${DPDK_SRC_DIR}" status --porcelain)" ]] || { echo "DPDK source has local changes: ${DPDK_SRC_DIR}" >&2; exit 1; }

		pushd "${DPDK_SRC_DIR}" >/dev/null
		git fetch --tags origin
		git checkout "v${DPDK_VER}"
		if compgen -G "${REPO_DIR}/patches/dpdk/${DPDK_VER}/*.patch" >/dev/null; then
			git am "${REPO_DIR}"/patches/dpdk/"${DPDK_VER}"/*.patch
		fi
		meson setup build --wipe
		ninja -C build
		run_as_root ninja install -C build
		popd >/dev/null
	fi

	pushd "${REPO_DIR}" >/dev/null
	./build.sh "${BUILDTYPE}"
	popd >/dev/null
}

case "${DRIVER}" in
ice) build_ice ;;
igc) build_igc ;;
*) echo "Unsupported driver '${DRIVER}'. Use ice or igc." >&2; exit 1 ;;
esac
