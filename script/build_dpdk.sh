#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Run from top, for CI job
dpdk_ver=22.11

# check out dpdk code
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v$dpdk_ver
git switch -c v$dpdk_ver

# apply the patches
git am ../patches/dpdk/$dpdk_ver/*.patch

# build and install dpdk now
meson build
ninja -C build
cd build
sudo ninja install