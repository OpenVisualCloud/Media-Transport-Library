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

Compute deterministic SHA-256 source checksums for CI cache keys.
Paths for each component are defined in script/hash_sources_*.env files.
The paths are relative to the repository root, so the script reads them
from the repository root. The checksums do not change with the current
directory.

REQUIRED PACKAGES (Debian/Ubuntu):
	coreutils findutils grep

OPTIONS:
	-o FILE		Add KEY=VALUE lines to FILE (for GITHUB_OUTPUT)
	-h		Show this help message

EXAMPLES:
	${script_name}				# Print checksums to stdout
	${script_name} -o \$GITHUB_OUTPUT	# Write checksums for GitHub Actions
	${script_name} -o /dev/stdout		# Print KEY=VALUE lines, then the table
EOF
}

# Read paths from an .env file (skip comments and blank lines).
read_env() {
	local env_file="$1"
	grep -v '^\s*#' "$env_file" | grep -v '^\s*$'
}

# List the regular files under the given paths. In a git work tree, list only
# the files that git tracks: a build writes untracked files into some of the
# paths (lib/, app/, manager/ ...), and the hash must not change after a build.
# A symbolic link is not a regular file, as for find -type f.
list_files() {
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		git ls-files -s -z -- "$@" | while IFS= read -r -d '' record; do
			[[ "$record" == 120000* || "$record" == 160000* ]] && continue
			[ -f "${record#*$'\t'}" ] && printf '%s\n' "${record#*$'\t'}"
		done
	else
		find "$@" -type f 2>/dev/null
	fi
}

# Hash all regular files under the given paths, sorted for determinism.
hash_paths() {
	local files
	files="$(list_files "$@" | LC_ALL=C sort || true)"
	if [ -z "$files" ]; then
		printf '%s' "" | sha256sum | cut -d' ' -f1
	else
		printf '%s\n' "$files" | xargs sha256sum | sha256sum | cut -d' ' -f1
	fi
}

# Hash an arbitrary string (used to chain parent hashes into children).
hash_string() {
	printf '%s' "$1" | sha256sum | cut -d' ' -f1
}

# Prints the hash of the paths in script/hash_sources_$1.env. The .env file
# gives each line to find as one unquoted word
# (.github/scripts/ci/check-path-filters.py checks the lines).
component_paths_hash() {
	# shellcheck disable=SC2046
	hash_paths $(read_env "${script_folder}/hash_sources_$1.env")
}

# Waterfall:
#   dpdk      = hash(dpdk_paths)
#   mtl       = hash(mtl_paths + dpdk_checksum)
#   jpegxs    = hash(jpegxs_paths + mtl_checksum)
#   ice       = hash(ice_paths)
#   ffmpeg    = hash(ffmpeg_paths + jpegxs_checksum)
#   gstreamer = hash(gstreamer_paths + mtl_checksum)
#   plugins   = hash(plugins_paths + ffmpeg_checksum)
main() {
	local opt output_env="" dpdk mtl jpegxs ice ffmpeg gstreamer plugins

	while getopts "ho:" opt; do
		case $opt in
		o)
			output_env="$OPTARG"
			;;
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
	[ "$#" -eq 0 ] || {
		log_error "Unexpected argument: $1"
		show_help
		exit 1
	}

	require_commands sha256sum:coreutils find:findutils xargs:findutils \
		grep:grep sort:coreutils cut:coreutils || exit 1

	# The .env paths are relative to the repository root. Keep a relative
	# -o FILE relative to the directory of the caller.
	if [ -n "$output_env" ] && [[ $output_env != /* ]]; then
		output_env="${PWD}/${output_env}"
	fi
	cd "${REPO_DIR}"

	dpdk="$(hash_string "$(component_paths_hash dpdk)")"
	mtl="$(hash_string "${dpdk} $(component_paths_hash mtl)")"
	jpegxs="$(hash_string "${mtl} $(component_paths_hash jpegxs)")"
	ice="$(hash_string "$(component_paths_hash ice)")"
	ffmpeg="$(hash_string "${jpegxs} $(component_paths_hash ffmpeg)")"
	gstreamer="$(hash_string "${mtl} $(component_paths_hash gstreamer)")"
	plugins="$(hash_string "${ffmpeg} $(component_paths_hash plugins)")"

	if [ -n "$output_env" ]; then
		{
			echo "dpdk=${dpdk}"
			echo "mtl=${mtl}"
			echo "jpegxs=${jpegxs}"
			echo "ice=${ice}"
			echo "ffmpeg=${ffmpeg}"
			echo "gstreamer=${gstreamer}"
			echo "plugins=${plugins}"
		} >>"$output_env"
	fi

	printf '  %-20s %s\n' \
		"dpdk:" "${dpdk}" \
		"mtl:" "${mtl}" \
		"jpegxs:" "${jpegxs}" \
		"ice:" "${ice}" \
		"ffmpeg:" "${ffmpeg}" \
		"gstreamer:" "${gstreamer}" \
		"plugins:" "${plugins}"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
