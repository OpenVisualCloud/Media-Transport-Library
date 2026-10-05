#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"
media_dir=${2:-/mnt/media}

case "${MEDIA_ASSET_SET:-smoke}" in
smoke) assets="yuv_files_422p10le:Penguin_1080p audio_files:PCM24 anc_files:text_p59" ;;
perf) assets="yuv_files_422rfc10:ParkJoy_1080p_24frames yuv_files_422rfc10:ParkJoy_4K_24frames yuv_files_422rfc10:Penguin_8K_24frames" ;;
*)
	echo "MEDIA_ASSET_SET is ${MEDIA_ASSET_SET}; expected smoke or perf." >&2
	exit 2
	;;
esac

# One line per asset: group filename format width height fps parent prefix_bytes
# shellcheck disable=SC2086,SC2154 # word-split asset list; venv_python from the sourced file
meta=$(
	cd "${root_dir}/tests/acceptance" && "${venv_python}" - ${assets} <<-'PY'
		import re
		import sys

		from mtl_engine import media_files
		from mtl_engine.integrity import calculate_yuv_frame_size

		for asset in sys.argv[1:]:
		    group, _, key = asset.partition(":")
		    info = getattr(media_files, group)[key]
		    parent, size = "-", 0
		    if group == "yuv_files_422rfc10":
		        frames = int(re.search(r"_(\d+)frames\.yuv$", info["filename"]).group(1))
		        parent = getattr(media_files, group)[key.removesuffix(f"_{frames}frames")]["filename"]
		        size = frames * calculate_yuv_frame_size(info["width"], info["height"], info["file_format"])
		    fmt = info.get("file_format", info.get("format", "-"))
		    print(group, info["filename"], fmt, info.get("width", 0), info.get("height", 0), info.get("fps", "-"), parent, size)
	PY
)

as_root() { if ((EUID)); then sudo "$@"; else "$@"; fi; }

case "${1:-}" in
list | verify)
	missing=0
	while read -r _ filename _; do
		if [[ -s ${media_dir}/${filename} ]]; then
			printf '%-12s %s\n' present "${media_dir}/${filename}"
		else
			printf '%-12s %s\n' missing "${media_dir}/${filename}"
			missing=1
		fi
	done <<<"${meta}"
	if [[ ${1} == verify && ${missing} -eq 1 ]]; then
		echo "Media the suites read is missing from ${media_dir}." >&2
		echo "Mount the lab share there, or on a host without one: task ci:media-assets -- generate" >&2
		exit 1
	fi
	;;
generate)
	echo "Generating ${MEDIA_ASSET_SET:-smoke} test media in ${media_dir}"
	as_root mkdir -p "${media_dir}"
	while read -r -u 3 group filename file_format width height fps parent bytes; do
		path="${media_dir}/${filename}"
		if [[ -s ${path} ]]; then
			echo "  have     ${filename}"
			continue
		fi
		case "${group}" in
		yuv_files_422rfc10)
			# No ffmpeg encoder emits RFC4175 packed pixels, so a perf source is cut from the lab's full asset.
			if [[ ! -s ${media_dir}/${parent} ]]; then
				echo "Cannot cut ${filename}: ${media_dir}/${parent} is absent." >&2
				echo "Mount the lab share at ${media_dir} and retry." >&2
				exit 1
			fi
			echo "  cut      ${filename} (${bytes} B of ${parent})"
			as_root dd "if=${media_dir}/${parent}" "of=${path}" bs=1M \
				iflag=count_bytes "count=${bytes}" status=none
			;;
		yuv_files_422p10le)
			echo "  generate ${filename} (${width}x${height} ${file_format} x180)"
			ffmpeg=${root_dir}/.local_install/ffmpeg/bin/ffmpeg
			[[ -x ${ffmpeg} ]] || ffmpeg=$(command -v ffmpeg)
			ld_path=$(GITHUB_ENV=/dev/stdout GITHUB_PATH=/dev/null \
				bash "${root_dir}/.github/scripts/ci/configure-host.sh" environment |
				sed -n 's/^LD_LIBRARY_PATH=//p')
			as_root env "LD_LIBRARY_PATH=${ld_path}" "${ffmpeg}" -hide_banner -loglevel error -y \
				-f lavfi -i "testsrc=s=${width}x${height}:r=${fps}" \
				-frames:v 180 -pix_fmt yuv422p10le -f rawvideo "${path}"
			;;
		audio_files)
			echo "  generate ${filename} (48kHz 24ch 24-bit, 60s)"
			as_root dd if=/dev/urandom "of=${path}" bs=1M \
				count=$((48000 * 24 * 3 * 60 / 1024 / 1024)) status=none
			;;
		anc_files)
			echo "  generate ${filename} (ancillary text)"
			seq -f 'MTL ancillary data line %g' 1 256 | as_root tee "${path}" >/dev/null
			;;
		esac
	done 3<<<"${meta}"
	;;
*)
	echo "Usage: $0 {list|verify|generate} [dir]" >&2
	exit 2
	;;
esac
