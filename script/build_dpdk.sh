#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

# Run from top, for CI job
if [ -n "$1" ];  then
  dpdk_ver=$1
else
  # default to latest 22.11
  dpdk_ver=22.11
fi

echo "DPDK version: $dpdk_ver"

if [ ! -d "dpdk" ];then
  # check out dpdk code
  echo "Clone DPDK source code"
  git clone https://github.com/DPDK/dpdk.git
fi
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