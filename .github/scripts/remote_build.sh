#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
#
# Build MTL on a remote (non-GH-Actions) host.
# Piped via SSH from .github/actions/build/action.yml with env vars
# prepended from ~/.mtl_build_env on the GH Actions runner.
#
# Usage (via action.yml — not called directly):
#   { cat ~/.mtl_build_env; cat remote_build.sh; } | ssh host bash -s -- <mtl_path> <branch>

set -ex

MTL_DIR="${1:?missing mtl_path argument}"
BRANCH="${2:?missing branch argument}"

# Defaults — overridden by env vars prepended from the runner's ~/.mtl_build_env
export CICD_BUILD=${CICD_BUILD:-1}
export SETUP_BUILD_AND_INSTALL_DPDK=${SETUP_BUILD_AND_INSTALL_DPDK:-0}
export SETUP_BUILD_AND_INSTALL_ICE_DRIVER=${SETUP_BUILD_AND_INSTALL_ICE_DRIVER:-0}
export SETUP_BUILD_AND_INSTALL_EBPF_XDP=${SETUP_BUILD_AND_INSTALL_EBPF_XDP:-0}
export SETUP_BUILD_AND_INSTALL_GPU_DIRECT=${SETUP_BUILD_AND_INSTALL_GPU_DIRECT:-0}
export MTL_BUILD_AND_INSTALL=${MTL_BUILD_AND_INSTALL:-1}
export ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN=${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:-1}
export ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN=${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:-1}
export TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET=${TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET:-1}
export TOOLS_RUN_SET_TAI_OFFSET=${TOOLS_RUN_SET_TAI_OFFSET:-1}

# ── Sync repo ──
cd "$MTL_DIR"
git config --global --add safe.directory "$MTL_DIR"
git fetch --all --prune
git checkout "origin/$BRANCH"
git reset --hard "origin/$BRANCH"

# ── Build ──
bash .github/scripts/setup_environment.sh
