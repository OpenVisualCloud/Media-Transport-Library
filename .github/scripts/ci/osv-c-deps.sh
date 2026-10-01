#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Print the C/C++ components that MTL builds from source as an OSV-Scanner
# inventory. No scanner reads versions.env or the build scripts, so this list
# is how OSV-Scanner sees them. After changing a pin below:
#   .github/scripts/ci/osv-c-deps.sh >.github/osv/c-deps.json
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"

pin() { # <owner/repo> <tag or commit>
	local commit=$2
	if [[ ! $2 =~ ^[0-9a-f]{40}$ ]]; then
		# An annotated tag lists the commit it points at on a second, peeled line
		commit=$(git ls-remote "https://github.com/$1" "refs/tags/$2" "refs/tags/$2^{}" | tail -1 | cut -f1)
		[ -n "$commit" ] || {
			echo "$1 has no tag $2" >&2
			exit 1
		}
	fi
	jq -cn --arg name "https://github.com/$1" --arg commit "$commit" \
		'{package: {name: $name, commit: $commit}}'
}

{
	pin DPDK/dpdk "v${DPDK_VER}"
	pin libbpf/libbpf "v${EBPF_VER}"
	pin intel/ethernet-linux-ice "v${ICE_VER}"
	pin xdp-project/xdp-tools "v${XDP_TOOLS_VER}"
	pin cisco/openh264 "v${OPENH264_VER}"
	pin OpenVisualCloud/SVT-JPEG-XS "${SVT_JPEG_XS_VER}"
	pin oneapi-src/level-zero "v${ONE_API_GPU_VER}"
	# Pinned outside versions.env: doc/build.md, gpu_direct/subprojects/gtest.wrap
	# and setup_environment.sh. Sources that follow a branch have no version to
	# list: FFmpeg, xdp-tools in manager/Dockerfile, json-c and googletest in
	# doc/build.md.
	pin the-tcpdump-group/libpcap libpcap-1.10.7
	pin google/googletest v1.15.0
	pin swig/swig v4.1.1
} | jq -s '{results: [{packages: .}]}'
