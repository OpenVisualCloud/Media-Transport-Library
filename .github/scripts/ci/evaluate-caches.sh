#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
components=(dpdk mtl jpegxs ffmpeg gstreamer plugins ice)
any_miss=0

for component in "${components[@]}"; do
	upper=${component^^}
	hit_var="CACHE_HIT_${upper}"
	hit=${!hit_var:-false}
	miss=1
	if [ "$hit" = true ]; then
		if bash "${root_dir}/.github/scripts/ci/validate-cache.sh" "$component"; then
			miss=0
		else
			echo "::warning::${component} cache is invalid and will be rebuilt"
			rm -rf "${root_dir}/.local_install/${component}"
		fi
	fi
	[ "$miss" -eq 1 ] && any_miss=1
	echo "CI_BUILD_${upper}=${miss}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	echo "${component}_miss=${miss}" >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
	echo "::notice::${component}=$([ "$miss" -eq 1 ] && echo MISS || echo HIT)"
done

echo "any_miss=${any_miss}" >>"$GITHUB_OUTPUT"