#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

# Test media for a host that has no asset share.
#
# Fleet runners mount the lab's share at media_path (/mnt/media), and the
# acceptance tests skip -- by design -- whatever is not there. On a host without
# that share every media-carrying test skips, so a run comes back green having
# transmitted nothing. This generates the assets the smoke suites open, under the
# names and geometries media_files.py declares. The content is synthetic, which
# is all a transport test needs: it moves the bytes and compares them back.
#
# Usage: media-assets.sh {list|verify|generate} [dir]  (dir defaults to /mnt/media)
#        MEDIA_ASSET_SET={smoke|perf} picks which set to act on (default smoke).

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"
# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"
media_dir=${2:-/mnt/media}

# The assets the smoke and low_bandwidth suites read, as <dict>:<key> into
# tests/acceptance/mtl_engine/media_files.py. Only the selection lives here: the
# filename, geometry, frame rate and format come from that file, so this cannot
# drift from what the tests actually open.
ASSETS=(
	yuv_files_422p10le:Penguin_1080p # st20p and st22p, the low-rate cases
	audio_files:PCM24                # st30p; PCM8/16/24 share one raw file
	anc_files:text_p59               # st40p
)

# The perf sweep reads short prefixes of the full-length assets instead, so that
# every session's source fits in hugepages. See PERF_SOURCE_FRAMES in
# tests/acceptance/tests/dual/performance/test_vf_perf_dualhost.py for why.
# These are the ones its cases open; a host missing them skips the whole leg.
PERF_ASSETS=(
	yuv_files_422rfc10:ParkJoy_1080p_24frames # the 1080p sweep
	yuv_files_422rfc10:ParkJoy_4K_24frames    # the 2160p sweep
	yuv_files_422rfc10:Penguin_8K_24frames    # the 4320p sweep
)

case "${MEDIA_ASSET_SET:=smoke}" in
smoke) SELECTED=("${ASSETS[@]}") ;;
perf) SELECTED=("${PERF_ASSETS[@]}") ;;
*)
	echo "MEDIA_ASSET_SET is ${MEDIA_ASSET_SET}; expected smoke or perf." >&2
	exit 2
	;;
esac

# The reference video asset is 180 frames. The apps loop their input, so this
# bounds the file size, not the length of a test.
FRAMES=180

asset_meta() {
	# asset_meta <dict>:<key> -> "<filename> <file_format> <width> <height> <fps>"
	# shellcheck disable=SC2154 # venv_python comes from mtl_acceptance_venv.sh, sourced above
	[[ -x ${venv_python} ]] || {
		echo "Missing acceptance virtualenv at ${venv_python}." >&2
		echo "Provision it with: task ci:pytest-setup -- install" >&2
		return 1
	}
	(
		cd "${acceptance_dir}" && "${venv_python}" - "$1" <<-'PY'
			import sys
			from mtl_engine import media_files

			group, _, key = sys.argv[1].partition(":")
			info = getattr(media_files, group)[key]
			print(
			    info["filename"],
			    info.get("file_format", info.get("format", "-")),
			    info.get("width", 0),
			    info.get("height", 0),
			    info.get("fps", "-"),
			)
		PY
	)
}

prefix_meta() {
	# prefix_meta <dict>:<key> -> "<parent filename> <bytes>", for a perf prefix.
	#
	# The frame count is read out of the asset's own name and the parent out of
	# its own key, so this file holds no second copy of either, and the
	# per-frame arithmetic stays in the module the tests themselves use for it.
	(
		cd "${acceptance_dir}" && "${venv_python}" - "$1" <<-'PY'
			import re
			import sys

			from mtl_engine import media_files
			from mtl_engine.integrity import calculate_yuv_frame_size

			group, _, key = sys.argv[1].partition(":")
			assets = getattr(media_files, group)
			info = assets[key]
			frames = int(re.search(r"_(\d+)frames\.yuv$", info["filename"]).group(1))
			parent = assets[re.sub(rf"_{frames}frames$", "", key)]
			print(
			    parent["filename"],
			    frames
			    * calculate_yuv_frame_size(
			        info["width"], info["height"], info["file_format"]
			    ),
			)
		PY
	)
}

ffmpeg_bin() {
	# The MTL build's FFmpeg is the one the tests themselves use, and unlike a
	# distribution one it is certain to be there on a host that has built MTL.
	if [[ -x ${root_dir}/.local_install/ffmpeg/bin/ffmpeg ]]; then
		echo "${root_dir}/.local_install/ffmpeg/bin/ffmpeg"
	elif command -v ffmpeg >/dev/null 2>&1; then
		command -v ffmpeg
	else
		echo "No ffmpeg: build it (task build) or install one." >&2
		return 1
	fi
}

load_job_environment() {
	# The MTL FFmpeg resolves its libraries through the environment the test jobs
	# are handed. This script may run before the job that sets it, or outside a
	# job entirely, so ask the same task for that environment instead of keeping
	# a second copy of the paths here.
	local env_file path_file
	env_file=$(mktemp)
	path_file=$(mktemp)
	GITHUB_ENV="${env_file}" GITHUB_PATH="${path_file}" \
		bash "${root_dir}/.github/scripts/ci/configure-host.sh" environment
	set -a
	# shellcheck disable=SC1090 # written just above
	source "${env_file}"
	set +a
	PATH="$(paste -sd: "${path_file}"):${PATH}"
	rm -f "${env_file}" "${path_file}"
}

as_root() {
	# The asset directory belongs to root on a fleet host, and the tests copy out
	# of it as root, so write it the same way rather than loosening it.
	if [[ $(id -u) -eq 0 ]]; then "$@"; else sudo "$@"; fi
}

generate_yuv() {
	# generate_yuv <path> <pix_fmt> <width> <height> <fps>
	local path=$1 pix_fmt=$2 width=$3 height=$4 fps=$5 ffmpeg
	ffmpeg=$(ffmpeg_bin)
	load_job_environment
	as_root env "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}" "${ffmpeg}" -hide_banner -loglevel error -y \
		-f lavfi -i "testsrc=s=${width}x${height}:r=${fps}" \
		-frames:v "${FRAMES}" -pix_fmt "${pix_fmt}" -f rawvideo "${path}"
}

generate_asset() {
	local asset=$1 group=${1%%:*} filename file_format width height fps path
	local parent bytes
	read -r filename file_format width height fps < <(asset_meta "${asset}")
	path="${media_dir}/${filename}"
	if [[ -s ${path} ]]; then
		echo "  have     ${filename}"
		return 0
	fi
	case "${group}" in
	yuv_files_422rfc10)
		# A perf source is a byte prefix of the full-length asset: the apps
		# advance the read cursor one frame at a time and wrap at the end, so
		# any whole number of frames is itself a valid source. Cutting keeps
		# the real content, and no ffmpeg encoder emits RFC4175 packed pixels
		# anyway. The parent has to be on the share already -- this derives
		# from the lab's media, it does not stand in for it.
		read -r parent bytes < <(prefix_meta "${asset}")
		if [[ ! -s ${media_dir}/${parent} ]]; then
			echo "Cannot cut ${filename}: ${media_dir}/${parent} is absent." >&2
			echo "Mount the lab share at ${media_dir} and retry." >&2
			return 1
		fi
		echo "  cut      ${filename} (${bytes} B of ${parent})"
		as_root dd "if=${media_dir}/${parent}" "of=${path}" bs=1M \
			iflag=count_bytes "count=${bytes}" status=none
		;;
	yuv_files_422p10le)
		echo "  generate ${filename} (${width}x${height} ${file_format} x${FRAMES})"
		generate_yuv "${path}" yuv422p10le "${width}" "${height}" "${fps}"
		;;
	audio_files)
		# 48 kHz, 24 channels, 24-bit, one minute -- the shape the canonical
		# asset's name states, which is what st30p reads it as. Noise: the
		# integrity check compares transmitted bytes to received ones.
		echo "  generate ${filename} (48kHz 24ch 24-bit, 60s)"
		as_root dd if=/dev/urandom "of=${path}" bs=1M \
			count=$((48000 * 24 * 3 * 60 / 1024 / 1024)) status=none
		;;
	anc_files)
		# ST 2110-40 carries the file's bytes as ancillary payload; the reference
		# asset is a short text file.
		echo "  generate ${filename} (ancillary text)"
		seq -f 'MTL ancillary data line %g' 1 256 | as_root tee "${path}" >/dev/null
		;;
	*)
		echo "No generator for ${asset} (${file_format}); mount the asset share." >&2
		return 1
		;;
	esac
}

case "${1:-}" in
list | verify)
	# verify is list with a verdict, for a job to run before the suite. The
	# suite itself skips a case whose media file is absent, by design, so a host
	# with no share comes back green having transmitted nothing -- the one
	# failure mode a green run cannot tell you about.
	missing=0
	for asset in "${SELECTED[@]}"; do
		read -r filename _ < <(asset_meta "${asset}")
		if [[ -s ${media_dir}/${filename} ]]; then
			printf '%-12s %s\n' present "${media_dir}/${filename}"
		else
			printf '%-12s %s\n' missing "${media_dir}/${filename}"
			missing=1
		fi
	done
	if [[ ${1} == verify && ${missing} -eq 1 ]]; then
		echo "Media the smoke suites read is missing from ${media_dir}." >&2
		echo "Mount the lab share there, or on a host without one: task ci:media-assets -- generate" >&2
		exit 1
	fi
	;;
generate)
	echo "Generating smoke test media in ${media_dir}"
	as_root mkdir -p "${media_dir}"
	for asset in "${SELECTED[@]}"; do
		generate_asset "${asset}"
	done
	;;
*)
	echo "Usage: $0 {list|verify|generate} [dir]" >&2
	exit 2
	;;
esac
