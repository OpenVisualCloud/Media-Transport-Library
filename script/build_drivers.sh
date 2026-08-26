#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
# shellcheck disable=SC1091
. "${REPO_DIR}/script/common.sh"

BUILD_ICE=true
BUILD_IGC=true
BUILD_IAVF=true
BUILD_ONLY=false
FORCE=false
if [[ "${FORCE_ICE_REBUILD:-0}" == "1" || "${FORCE_IAVF_REBUILD:-0}" == "1" ]]; then
	FORCE=true
fi
usage() {
	cat <<USAGE
Usage: $(basename "$0") [OPTIONS]

Build drivers used by Media Transport Library.
By default, all driver flows are built.

Options:
  --driver <ice|igc|iavf>    Build only the selected driver flow
  --disable-ice              Do not build the ICE driver flow
  --disable-igc              Do not build the IGC driver flow
  --disable-iavf             Do not build the IAVF driver flow
  --build-only               Compile ICE/IAVF and leave the built .ko in place,
                             without installing or loading it
  --ice-version <version>    ICE version (default: ${ICE_VER})
  --ice-download-id <id>     Intel download mirror ID for ICE (default: ${ICE_DMID})
  --iavf-version <version>   IAVF version (default: ${IAVF_VER})
  --iavf-download-id <id>    Intel download mirror ID for IAVF (default: ${IAVF_DMID})
	--force                    Rebuild ICE/IAVF
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
		[[ $# -ge 2 ]] || {
			echo "--driver requires a value" >&2
			exit 1
		}
		case "$2" in
		ice)
			BUILD_ICE=true
			BUILD_IGC=false
			BUILD_IAVF=false
			;;
		igc)
			BUILD_ICE=false
			BUILD_IGC=true
			BUILD_IAVF=false
			;;
		iavf)
			BUILD_ICE=false
			BUILD_IGC=false
			BUILD_IAVF=true
			;;
		*)
			echo "Unsupported driver '$2'. Use ice, igc or iavf." >&2
			exit 1
			;;
		esac
		shift 2
		;;
	--disable-ice)
		BUILD_ICE=false
		shift
		;;
	--disable-igc)
		BUILD_IGC=false
		shift
		;;
	--disable-iavf)
		BUILD_IAVF=false
		shift
		;;
	--build-only)
		BUILD_ONLY=true
		shift
		;;
	--ice-version)
		[[ $# -ge 2 ]] || {
			echo "--ice-version requires a value" >&2
			exit 1
		}
		ICE_VER="$2"
		shift 2
		;;
	--ice-download-id)
		[[ $# -ge 2 ]] || {
			echo "--ice-download-id requires a value" >&2
			exit 1
		}
		ICE_DMID="$2"
		shift 2
		;;
	--iavf-version)
		[[ $# -ge 2 ]] || {
			echo "--iavf-version requires a value" >&2
			exit 1
		}
		IAVF_VER="$2"
		shift 2
		;;
	--iavf-download-id)
		[[ $# -ge 2 ]] || {
			echo "--iavf-download-id requires a value" >&2
			exit 1
		}
		IAVF_DMID="$2"
		shift 2
		;;
	--force)
		FORCE=true
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
	local patch_root="${REPO_DIR}/patches/ice_drv"
	local patch_dir="${patch_root}/${ICE_VER}"
	local github_archive=0

	if [[ ! -d "${patch_dir}" ]]; then
		echo "No ICE patches for version ${ICE_VER}." >&2
		echo "Directory does not exist: ${patch_dir}" >&2
		echo "Available ICE versions: $(find -L "${patch_root}" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -V | paste -sd' ')" >&2
		exit 1
	fi

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
		if [[ "${FORCE}" == "true" || "${BUILD_ONLY}" == "true" ]]; then
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
	[[ -d "ice-${ICE_VER}" ]] || {
		echo "Failed to extract ${archive_name}." >&2
		exit 1
	}

	pushd "ice-${ICE_VER}" >/dev/null
	for patch_file in "${REPO_DIR}"/patches/ice_drv/"${ICE_VER}"/*.patch; do
		patch -p1 -i "${patch_file}"
	done
	make -C src -j"$(nproc)" CC="${CC:-cc}"
	if [[ "${BUILD_ONLY}" == "false" ]]; then
		run_as_root make -C src install
		run_as_root rmmod irdma || true
		run_as_root rmmod ice
		run_as_root modprobe ice
	fi
	popd >/dev/null
	# --build-only asked for the module, not an installed driver, so leave it
	# where the caller can collect it. Anyone who wanted it installed has it
	# installed and does not need the tree.
	if [[ "${BUILD_ONLY}" == "true" ]]; then
		echo "Built ${SCRIPT_DIR}/ice-${ICE_VER}/src/ice.ko"
	else
		rm -rf "ice-${ICE_VER}"
	fi
}

build_iavf() {
	local archive_name="iavf-${IAVF_VER}.tar.gz"
	local patch_dir="${REPO_DIR}/patches/iavf_drv/${IAVF_VER}"
	local github_archive=0

	if [[ "${BUILD_ONLY}" == "false" && "${FORCE}" == "false" ]] &&
		sudo modinfo iavf 2>/dev/null | grep -Ei "^version:[[:space:]]*(Kahawai_)?${IAVF_VER}([[:space:]]|$)" >/dev/null; then
		echo "IAVF driver version ${IAVF_VER} is already installed. Skipping rebuild."
		return
	fi

	cd "${SCRIPT_DIR}"
	if [[ -f "${archive_name}" ]] && gzip -t "${archive_name}" >/dev/null 2>&1; then
		echo "Found valid local archive ${archive_name}, skipping download."
		if tar -tzf "${archive_name}" | grep "^ethernet-linux-iavf" >/dev/null; then
			github_archive=1
		fi
	else
		rm -f "${archive_name}"
		wget "https://downloadmirror.intel.com/${IAVF_DMID}/${archive_name}" -O "${archive_name}" || true
		if [[ ! -f "${archive_name}" ]] || ! gzip -t "${archive_name}" >/dev/null 2>&1; then
			rm -f "${archive_name}"
			wget "https://github.com/intel/ethernet-linux-iavf/archive/refs/tags/v${IAVF_VER}.tar.gz" -O "${archive_name}" || true
			if [[ -f "${archive_name}" ]] && gzip -t "${archive_name}" >/dev/null 2>&1; then
				github_archive=1
			else
				echo "Failed to download a valid ${archive_name}." >&2
				rm -f "${archive_name}"
				exit 1
			fi
		fi
	fi

	if [[ -d "iavf-${IAVF_VER}" ]]; then
		if [[ "${FORCE}" == "true" || "${BUILD_ONLY}" == "true" ]]; then
			rm -rf "iavf-${IAVF_VER}"
		else
			echo "iavf-${IAVF_VER} already exists. Use --force to replace it." >&2
			exit 1
		fi
	fi

	tar xzf "${archive_name}"
	rm -f "${archive_name}"
	if [[ "${github_archive}" -eq 1 && -d "ethernet-linux-iavf-${IAVF_VER}" ]]; then
		mv "ethernet-linux-iavf-${IAVF_VER}" "iavf-${IAVF_VER}"
	fi
	[[ -d "iavf-${IAVF_VER}" ]] || {
		echo "Failed to extract ${archive_name}." >&2
		exit 1
	}

	pushd "iavf-${IAVF_VER}" >/dev/null
	if [[ -d "${patch_dir}" ]]; then
		shopt -s nullglob
		for patch_file in "${patch_dir}"/*.patch; do
			patch -p1 --no-backup-if-mismatch -i "${patch_file}"
		done
		shopt -u nullglob
	fi
	make -C src -j"$(nproc)" CC="${CC:-cc}"
	if [[ "${BUILD_ONLY}" == "false" ]]; then
		run_as_root make -C src install
		run_as_root rmmod iavf || true
		run_as_root modprobe iavf
	fi
	popd >/dev/null
	# --build-only asked for the module, not an installed driver, so leave it
	# where the caller (build-ice.sh packages it alongside ice.ko) can collect it.
	if [[ "${BUILD_ONLY}" == "true" ]]; then
		echo "Built ${SCRIPT_DIR}/iavf-${IAVF_VER}/src/iavf.ko"
	else
		rm -rf "iavf-${IAVF_VER}"
	fi
}

build_igc() {
	if modinfo igc >/dev/null 2>&1; then
		echo "In-tree IGC driver is already installed at $(modinfo -n igc)."
		return
	fi

	if [[ "${BUILD_ONLY}" == "true" ]]; then
		echo "BUILD_ONLY is true. Skipping IGC driver installation." >&2
		return
	fi

	if [[ -z "${DRIVER_PACKAGE}" ]]; then
		echo "No IGC driver package configured for ${ID}." >&2
		exit 1
	fi

	echo "Installing in-tree IGC driver package ${DRIVER_PACKAGE} from the ${ID} repository."
	install_packages "${DRIVER_PACKAGE}"
	if ! modinfo igc >/dev/null 2>&1; then
		log_error "IGC driver is unavailable after installing ${DRIVER_PACKAGE}." >&2
		exit 1
	fi
}

if [[ "${BUILD_ICE}" == "false" && "${BUILD_IGC}" == "false" && "${BUILD_IAVF}" == "false" ]]; then
	echo "All driver flows are disabled." >&2
	exit 1
fi

if [[ "${BUILD_ICE}" == "true" ]]; then
	build_ice
fi
if [[ "${BUILD_IAVF}" == "true" ]]; then
	build_iavf
fi
if [[ "${BUILD_IGC}" == "true" ]]; then
	build_igc
fi
