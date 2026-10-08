#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Writes compile_commands.json for clang-tidy: libmtl with its unit and fuzz
# targets, and the meson projects that build on it. DPDK and MTL come from the
# Build workflow's caches in .local_install.
set -euo pipefail

# The flags add the GStreamer and OBS headers the meson setups below need. Only
# the install function runs: the script itself would also build both plugins.
export ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN=1 ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN=1
bash -c '. .github/scripts/setup_environment.sh && setup_ubuntu_install_dependencies'

# The .pc files name the prefix of the build host, and static libraries that
# this host does not have.
dpdk=$PWD/.local_install/dpdk/lib/x86_64-linux-gnu/pkgconfig
mtl=$PWD/.local_install/mtl/lib/x86_64-linux-gnu/pkgconfig
sed -i -e "s|^prefix=.*|prefix=\${pcfiledir}/../../..|" -e '/^Requires.private:/d' "$dpdk"/*.pc "$mtl"/*.pc
export PKG_CONFIG_PATH=$dpdk:$mtl CC=clang CXX=clang++

meson setup build -Denable_unit_tests=true -Denable_fuzzing=true
meson compile -C build usdt_provider_header
for project in app tests plugins ld_preload manager tests/tools/RxTxApp ecosystem/gstreamer_plugin \
	tests/tools/gstreamer_tools ecosystem/obs_mtl/linux-mtl tools/set_tai_offset; do
	meson setup "build/$(basename "$project")" "$project"
done
jq -s add build/compile_commands.json build/*/compile_commands.json >compile_commands.json
