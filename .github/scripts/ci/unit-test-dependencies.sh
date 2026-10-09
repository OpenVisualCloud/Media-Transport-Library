#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Build prerequisites for the unit tier, which needs no NIC and no root.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

# No sudo: the script sudos where it needs root, keeping the cache tree runner-owned.
bash "${root_dir}/.github/scripts/setup_environment.sh"
# tests/unit/gstreamer compiles against the GStreamer headers, which
# setup_environment.sh installs only for the GStreamer plugin build.
# libgstreamer1.0-dev needs libunwind-dev, which the runner image's libc++-14-dev
# blocks through libunwind-14-dev; naming it lets apt remove both.
sudo apt-get install -y --no-install-recommends libunwind-dev \
	libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
