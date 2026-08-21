#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

# The host's own prerequisites first: they are what a restored artifact is
# consumed with, and a missing libelf surfaces as an unrelated pkg-config
# failure deep inside a later build. The build scope, since these are the
# install trees a build produced, not xdp-tools.
bash "${root_dir}/script/build_ebpf_xdp.sh" --check build

components=(dpdk mtl jpegxs ffmpeg gstreamer plugins)
# The ICE bundle is only part of the environment on a host whose card is served
# by the ice driver; see ice-required.sh for why the NIC label decides that.
if [[ $(bash "${root_dir}/.github/scripts/ci/ice-required.sh") == true ]]; then
	components+=(ice)
fi

for component in "${components[@]}"; do
	bash "${root_dir}/.github/scripts/ci/validate-cache.sh" "$component"
done
