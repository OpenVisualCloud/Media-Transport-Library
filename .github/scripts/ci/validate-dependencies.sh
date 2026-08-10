#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

for component in dpdk mtl jpegxs ffmpeg gstreamer plugins ice; do
	bash "${root_dir}/.github/scripts/ci/validate-cache.sh" "$component"
done