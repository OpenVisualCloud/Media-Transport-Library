#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
# shellcheck source-path=SCRIPTDIR source=../../lib/mtl_acceptance_venv.sh disable=SC1091
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

case "${1:-}" in
verify)
	# shellcheck disable=SC2086,SC2154 # word-split asset list; venv_python from the sourced file
	filenames=$(cd "${root_dir}/tests/acceptance" && "${venv_python}" -c '
import sys
from mtl_engine import media_files
for asset in sys.argv[1:]:
    group, _, key = asset.partition(":")
    print(getattr(media_files, group)[key]["filename"])
' ${assets})
	missing=0
	while read -r filename; do
		if [[ -s ${media_dir}/${filename} ]]; then
			echo "present ${media_dir}/${filename}"
		else
			echo "missing ${media_dir}/${filename}"
			missing=1
		fi
	done <<<"${filenames}"
	exit "${missing}"
	;;
*)
	echo "Usage: $0 {verify} [dir]" >&2
	exit 2
	;;
esac
