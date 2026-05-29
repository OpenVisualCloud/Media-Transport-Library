#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
#
# Compute deterministic SHA-256 source checksums.
#
# Waterfall:
#   dpdk      = hash(dpdk_paths)
#   mtl       = hash(mtl_paths + dpdk_checksum)
#   ffmpeg    = hash(ffmpeg_paths + mtl_checksum)
#   gstreamer = hash(gstreamer_paths + mtl_checksum)
#   librist   = hash(librist_paths + mtl_checksum)

set -euo pipefail

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}

show_help() {
	cat <<EOF
Usage: $script_name [OPTIONS]

Compute deterministic SHA-256 source checksums for CI cache keys.
Paths for each component are defined in script/hash_sources_*.env files.

OPTIONS:
	-o FILE		Write KEY=VALUE lines to FILE (for GITHUB_OUTPUT)
	-h		Show this help message

EXAMPLES:
	$script_name				# Print checksums to stdout
	$script_name -o \$GITHUB_OUTPUT	# Write checksums for GitHub Actions
EOF
}

OUTPUT_ENV=""
while getopts "ho:" opt; do
	case $opt in
	o)
		OUTPUT_ENV="$OPTARG"
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

# ─── Helpers ────────────────────────────────────────────────────────────────

# Read paths from an .env file (skip comments and blank lines).
read_env() {
	local env_file="$1"
	grep -v '^\s*#' "$env_file" | grep -v '^\s*$'
}

# Hash all regular files under the given paths, sorted for determinism.
hash_paths() {
	local files
	files="$(find "$@" -type f 2>/dev/null | LC_ALL=C sort || true)"
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

# ─── Compute checksums ──────────────────────────────────────────────────────

# shellcheck disable=SC2046
dpdk_paths_hash="$(hash_paths $(read_env "${script_folder}/hash_sources_dpdk.env"))"
dpdk="$(hash_string "$dpdk_paths_hash")"

# shellcheck disable=SC2046
mtl_paths_hash="$(hash_paths $(read_env "${script_folder}/hash_sources_mtl.env"))"
mtl="$(hash_string "${dpdk} ${mtl_paths_hash}")"

# shellcheck disable=SC2046
ffmpeg_paths_hash="$(hash_paths $(read_env "${script_folder}/hash_sources_ffmpeg.env"))"
ffmpeg="$(hash_string "${mtl} ${ffmpeg_paths_hash}")"

# shellcheck disable=SC2046
gstreamer_paths_hash="$(hash_paths $(read_env "${script_folder}/hash_sources_gstreamer.env"))"
gstreamer="$(hash_string "${mtl} ${gstreamer_paths_hash}")"

# shellcheck disable=SC2046
librist_paths_hash="$(hash_paths $(read_env "${script_folder}/hash_sources_librist.env"))"
librist="$(hash_string "${mtl} ${librist_paths_hash}")"

# ─── Output ─────────────────────────────────────────────────────────────────

if [ -n "$OUTPUT_ENV" ]; then
	{
		echo "dpdk=${dpdk}"
		echo "mtl=${mtl}"
		echo "ffmpeg=${ffmpeg}"
		echo "gstreamer=${gstreamer}"
		echo "librist=${librist}"
	} >>"$OUTPUT_ENV"
fi

printf '  %-20s %s\n' \
	"dpdk:" "${dpdk}" \
	"mtl:" "${mtl}" \
	"ffmpeg:" "${ffmpeg}" \
	"gstreamer:" "${gstreamer}" \
	"librist:" "${librist}"
