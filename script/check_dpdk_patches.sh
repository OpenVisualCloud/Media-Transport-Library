#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

# GNU patch recovers a stale @@ header by searching for the hunk, applying it at
# an offset, and still exiting 0. So the exit code cannot detect a patch that has
# drifted from the tarball it was written against; the absence of an "offset" or
# "fuzz" line can.

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] [ARCHIVE_DIR]

Verify each patches/dpdk/<version>/*.patch applies to its pinned upstream
tarball with no hunk offset and no fuzz. ARCHIVE_DIR holds the archives, named
v<version>.zip as script/build_dpdk.sh downloads them. A version with no archive
in ARCHIVE_DIR is skipped, except the version versions.env pins: the run fails
unless the pin contributed at least one verified patch, so that a skip can never
read as a pass. Without ARCHIVE_DIR, the script downloads the archive of the
pinned version (${DPDK_VER}) to a temporary directory and checks only that
version. Prints nothing on stdout.

Only that flat series is covered, the same glob build_dpdk.sh applies. The
windows/ and hdr_split/ subdirectories are applied by other flows and are not
checked here, so drift in them goes unreported.

REQUIRED PACKAGES (Debian/Ubuntu):
	patch unzip wget (or curl, only without ARCHIVE_DIR)

OPTIONS:
	-h		Show this help message

EXAMPLES:
	${script_name}			# Download DPDK ${DPDK_VER} and check its patches
	${script_name} ~/dpdk-archives	# Check each version with an archive there
EOF
}

# Logs each line of $1 (the output of patch) as an indented error line.
print_detail() {
	local line
	while IFS= read -r line; do
		log_error "  ${line}"
	done <<<"$1"
}

# Downloads the archive of the pinned DPDK version into directory $1.
download_pinned_archive() {
	local url="https://github.com/DPDK/dpdk/archive/refs/tags/v${DPDK_VER}.zip"

	log_info "Download ${url}"
	if command_exists wget; then
		wget -q "${url}" -O "$1/v${DPDK_VER}.zip"
	else
		curl -fsSL "${url}" -o "$1/v${DPDK_VER}.zip"
	fi
}

main() {
	local opt patch_root archive_dir pinned_checked=0 failed=0 skipped=""
	local version_dir version archive tree patch_file patch_name output drift

	while getopts "h" opt; do
		case $opt in
		h)
			show_help
			exit 0
			;;
		*)
			show_help
			exit 1
			;;
		esac
	done
	shift $((OPTIND - 1))
	if [ "$#" -gt 1 ]; then
		log_error "Use one ARCHIVE_DIR, not $#."
		show_help
		exit 1
	fi

	# Without nullglob an empty patches/dpdk would be reported as a version named "*".
	shopt -s nullglob
	patch_root="${REPO_DIR}/patches/dpdk"

	require_commands patch:patch unzip:unzip || exit 1
	if [ "$#" -eq 0 ] && ! command_exists wget && ! command_exists curl; then
		require_commands wget:wget || exit 1
	fi

	if [ "$#" -eq 1 ]; then
		if ! archive_dir=$(readlink -qe "$1") || [ ! -d "${archive_dir}" ]; then
			log_error "no such directory: $1"
			exit 1
		fi
		if [ ! -f "${archive_dir}/v${DPDK_VER}.zip" ]; then
			log_error "versions.env pins '${DPDK_VER}', but ${archive_dir}/v${DPDK_VER}.zip is missing"
			exit 1
		fi
	fi

	# Global, because the EXIT trap runs after main returns.
	work_dir=$(mktemp -d)
	trap 'rm -rf "${work_dir}"' EXIT
	# bash resumes after a trap handler, so an interrupt must exit or it misreports.
	trap 'exit 130' INT TERM

	if [ "$#" -eq 0 ]; then
		archive_dir="${work_dir}/archives"
		mkdir -p "${archive_dir}"
		download_pinned_archive "${archive_dir}" || {
			log_error "Cannot download the DPDK ${DPDK_VER} archive."
			exit 1
		}
	fi

	for version_dir in "${patch_root}"/*/; do
		version=$(basename "${version_dir}")
		archive="${archive_dir}/v${version}.zip"
		if [ ! -f "${archive}" ]; then
			skipped="${skipped} ${version}"
			continue
		fi

		tree="${work_dir}/${version}"
		mkdir -p "${tree}"
		if ! unzip -q "${archive}" -d "${tree}"; then
			log_error "FAIL ${archive}: not a readable zip archive"
			failed=1
			rm -rf "${tree}"
			continue
		fi
		if [ ! -d "${tree}/dpdk-${version}" ]; then
			log_error "FAIL ${archive}: holds no dpdk-${version} directory"
			failed=1
			rm -rf "${tree}"
			continue
		fi

		for patch_file in "${version_dir}"*.patch; do
			if [ "${version}" = "${DPDK_VER}" ]; then
				pinned_checked=$((pinned_checked + 1))
			fi
			patch_name="${version}/$(basename "${patch_file}")"
			# Without --forward, --batch answers "Assume -R?" yes: silent pass, reversed tree.
			if ! output=$(patch --batch --forward -p1 -d "${tree}/dpdk-${version}" -i "${patch_file}" 2>&1); then
				log_error "FAIL ${patch_name}: does not apply"
				print_detail "${output}"
				failed=1
				continue
			fi
			# Only a +1 offset prints "line" singular; the ^Hunk anchor keeps file paths out.
			drift=$(grep -E '^Hunk #[0-9]+ .*(\(offset -?[0-9]+ lines?\)|with fuzz [0-9]+)' <<<"${output}" || true)
			if [ -n "${drift}" ]; then
				log_error "FAIL ${patch_name}: hunks do not land where the patch says"
				print_detail "${drift}"
				failed=1
			fi
		done

		# Each tree is ~129 MB, and every pinned version could be present at once.
		rm -rf "${tree}"
	done

	if [ -n "${skipped}" ]; then
		log_info "skipped, no archive in ${archive_dir}:${skipped}"
	fi

	if [ "${pinned_checked}" -eq 0 ]; then
		log_error "versions.env pins '${DPDK_VER}', but no patch under ${patch_root}/${DPDK_VER} was verified"
		exit 1
	fi

	[ "${failed}" -eq 0 ] || exit 1
	log_success "The DPDK ${DPDK_VER} patches apply with no offset and no fuzz."
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
