#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS]

Build drivers used by Media Transport Library.
By default, all driver flows are built.

REQUIRED PACKAGES (Debian/Ubuntu):
	gcc make patch tar gzip wget linux-headers-\$(uname -r)

OPTIONS:
	--driver <ice|igc>		Build only the selected driver flow
	--disable-ice			Do not build the ICE driver flow
	--disable-igc			Do not build the IGC driver flow
	--build-only			Compile ICE and leave the built .ko in place,
					without installing or loading it
	--ice-version <version>		ICE version (default: ${ICE_VER})
	--ice-download-id <id>		Intel download mirror ID (default: ${ICE_DMID})
	--force				Rebuild ICE
	-h, --help			Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX	Install into this tree instead of the system, as every
				build script of this repository does. A kernel module
				cannot go in a prefix and be loaded, so the .ko goes
				to <prefix parent>/ice/<kernel release>/<architecture>
				and nothing is installed or reloaded.
	FORCE_ICE_REBUILD	Set to 1 to rebuild ICE, as --force does

EXAMPLES:
	${script_name}				# Build and install all drivers
	${script_name} --driver igc		# Install the in-tree IGC driver only
	${script_name} --driver ice --force	# Rebuild and reload ICE
EOF
}

build_ice() {
	local archive_name="ice-${ICE_VER}.tar.gz"
	local patch_root="${REPO_DIR}/patches/ice_drv"
	local patch_dir="${patch_root}/${ICE_VER}"
	local github_archive=0
	local patch_file
	local tools=(make:make "${CC:-cc}:gcc" patch:patch tar:tar gzip:gzip)

	if [[ ! -d "${patch_dir}" ]]; then
		log_error "No ICE patches for version ${ICE_VER}."
		log_error "Directory does not exist: ${patch_dir}"
		log_error "Available ICE versions: $(find -L "${patch_root}" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -V | paste -sd' ')"
		exit 1
	fi

	if [[ "${BUILD_ONLY}" == "false" && "${FORCE}" == "false" ]] &&
		modinfo ice 2>/dev/null | grep -Ei "^version:[[:space:]]*Kahawai_${ICE_VER}" >/dev/null; then
		log_success "ICE driver version ${ICE_VER} (Kahawai) is already installed. Skipping rebuild."
		return
	fi

	cd "${script_folder}"
	if [[ ! -f "${archive_name}" ]] || ! gzip -t "${archive_name}" >/dev/null 2>&1; then
		tools+=(wget:wget)
	fi
	require_commands "${tools[@]}" || exit 1

	if [[ -f "${archive_name}" ]] && gzip -t "${archive_name}" >/dev/null 2>&1; then
		log_info "Found valid local archive ${archive_name}, skipping download."
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
				log_error "Failed to download a valid ${archive_name}."
				rm -f "${archive_name}"
				exit 1
			fi
		fi
	fi

	if [[ -d "ice-${ICE_VER}" ]]; then
		if [[ "${FORCE}" == "true" || "${BUILD_ONLY}" == "true" ]]; then
			rm -rf "ice-${ICE_VER}"
		else
			log_error "ice-${ICE_VER} already exists. Use --force to replace it."
			exit 1
		fi
	fi

	tar xzf "${archive_name}"
	rm -f "${archive_name}"
	if [[ "${github_archive}" -eq 1 && -d "ethernet-linux-ice-${ICE_VER}" ]]; then
		mv "ethernet-linux-ice-${ICE_VER}" "ice-${ICE_VER}"
	fi
	[[ -d "ice-${ICE_VER}" ]] || {
		log_error "Failed to extract ${archive_name}."
		exit 1
	}

	pushd "ice-${ICE_VER}" >/dev/null
	for patch_file in "${patch_dir}"/*.patch; do
		# --batch: a patch that does not fit this release must fail the build, not
		# stop on "File to patch:" and wait for a terminal nobody is watching.
		patch -p1 --batch --no-backup-if-mismatch -i "${patch_file}" || {
			log_error "Failed to apply $(basename "${patch_file}") to ice-${ICE_VER}."
			exit 1
		}
	done
	make -C src -j"${NPROC}" CC="${CC:-cc}"
	if [[ "${BUILD_ONLY}" == "false" ]]; then
		as_root make -C src install
		as_root rmmod irdma || true
		as_root rmmod ice
		as_root modprobe ice
	fi
	popd >/dev/null
	# A prefix takes the module, so the tree has done its job. Plain --build-only
	# leaves the tree where the caller can collect it, and an install has the
	# driver and does not need the tree either.
	if [[ -n "${BUNDLE_DIR}" ]]; then
		install -D -m 0644 "${script_folder}/ice-${ICE_VER}/src/ice.ko" "${BUNDLE_DIR}/ice.ko"
		log_success "Installed ${BUNDLE_DIR}/ice.ko"
		rm -rf "ice-${ICE_VER}"
	elif [[ "${BUILD_ONLY}" == "true" ]]; then
		log_success "Built ${script_folder}/ice-${ICE_VER}/src/ice.ko"
	else
		rm -rf "ice-${ICE_VER}"
		log_success "ICE driver ${ICE_VER} installed and loaded"
	fi
}

build_igc() {
	if modinfo igc >/dev/null 2>&1; then
		log_success "In-tree IGC driver is already installed at $(modinfo -n igc)."
		return
	fi

	if [[ "${BUILD_ONLY}" == "true" ]]; then
		log_warning "BUILD_ONLY is true. Skipping IGC driver installation."
		return
	fi

	if [[ -z "${DRIVER_PACKAGE}" ]]; then
		log_error "No IGC driver package configured for ${ID}."
		exit 1
	fi

	log_info "Installing in-tree IGC driver package ${DRIVER_PACKAGE} from the ${ID} repository."
	install_packages "${DRIVER_PACKAGE}"
	if ! modinfo igc >/dev/null 2>&1; then
		log_error "IGC driver is unavailable after installing ${DRIVER_PACKAGE}."
		exit 1
	fi
}

# Usage: need_value <option> <number of arguments left>
need_value() {
	[[ "$2" -ge 2 ]] && return 0
	log_error "$1 requires a value"
	show_help
	exit 1
}

main() {
	BUILD_ICE=true
	BUILD_IGC=true
	BUILD_ONLY=false
	FORCE=false
	if [[ "${FORCE_ICE_REBUILD:-0}" == "1" ]]; then
		FORCE=true
	fi

	while [[ $# -gt 0 ]]; do
		case "$1" in
		--driver)
			need_value "$1" "$#"
			case "$2" in
			ice)
				BUILD_ICE=true
				BUILD_IGC=false
				;;
			igc)
				BUILD_ICE=false
				BUILD_IGC=true
				;;
			*)
				log_error "Unsupported driver '$2'. Use ice or igc."
				show_help
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
		--build-only)
			BUILD_ONLY=true
			shift
			;;
		--ice-version)
			need_value "$1" "$#"
			ICE_VER="$2"
			shift 2
			;;
		--ice-download-id)
			need_value "$1" "$#"
			ICE_DMID="$2"
			shift 2
			;;
		--force)
			FORCE=true
			shift
			;;
		-h | --help)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown argument: $1"
			show_help
			exit 1
			;;
		esac
	done

	# The prefix is what makes the module the deliverable instead of the running
	# driver: the host that builds a .ko is not always the host that loads it, and CI
	# caches it per kernel release. Same sibling-directory shape as build_dpdk.sh.
	BUNDLE_DIR=""
	if [[ -n "${MTL_INSTALL_PREFIX:-}" ]]; then
		BUNDLE_DIR="$(dirname "${MTL_INSTALL_PREFIX}")/ice/$(uname -r)/$(uname -m)"
		BUILD_ONLY=true
	fi

	if [[ "${BUILD_ICE}" == "false" && "${BUILD_IGC}" == "false" ]]; then
		log_error "All driver flows are disabled."
		exit 1
	fi

	# Before the download: an out-of-tree module needs the headers of the kernel it is
	# built for, and make reports that as a missing target deep in the driver tree.
	if [[ "${BUILD_ICE}" == "true" ]]; then
		test -d "/lib/modules/$(uname -r)/build" || {
			log_error "kernel headers are missing for $(uname -r): install linux-headers-$(uname -r)"
			exit 1
		}
		build_ice
	fi
	if [[ "${BUILD_IGC}" == "true" ]]; then
		build_igc
	fi
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
