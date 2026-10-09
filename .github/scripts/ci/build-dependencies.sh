#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
local_install="${root_dir}/.local_install"

export MTL_INSTALL_PREFIX="${local_install}/mtl"
export SETUP_BUILD_AND_INSTALL_DPDK=${CI_BUILD_DPDK:-1}
export SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=${CI_BUILD_ICE:-1}
export MTL_BUILD_AND_INSTALL=${CI_BUILD_MTL:-1}
export ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN=${CI_BUILD_FFMPEG:-1}
export ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN=${CI_BUILD_GSTREAMER:-1}
export PLUGIN_BUILD_AND_INSTALL_AVCODEC=${CI_BUILD_PLUGINS:-1}
export PLUGIN_BUILD_AND_INSTALL_JPEGXS=${CI_BUILD_JPEGXS:-1}
export CICD_BUILD=1
export TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET=1
# Running it steps the host clock.
export TOOLS_RUN_SET_TAI_OFFSET=0
export PKG_CONFIG_PATH="${local_install}/jpegxs/lib/pkgconfig:${local_install}/jpegxs/lib64/pkgconfig:${local_install}/dpdk/lib/x86_64-linux-gnu/pkgconfig:${local_install}/mtl/lib/x86_64-linux-gnu/pkgconfig"
export LD_LIBRARY_PATH="${local_install}/jpegxs/lib:${local_install}/jpegxs/lib64:${local_install}/dpdk/lib/x86_64-linux-gnu:${local_install}/mtl/lib/x86_64-linux-gnu"

bash "${root_dir}/.github/scripts/setup_environment.sh"
