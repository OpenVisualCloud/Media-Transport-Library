#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Writes compile_commands.json for clang-tidy: libmtl with its unit and fuzz
# targets, and the meson projects that build on it. DPDK comes from the Build
# workflow's cache in .local_install; the projects find MTL through the
# mtl-uninstalled.pc that meson setup writes, so MTL is never built.
set -euo pipefail

# The components add the GStreamer and OBS headers the meson setups below need.
bash script/install_dependencies.sh gstreamer obs

# The .pc files name the prefix of the build host, and static libraries that
# this host does not have.
dpdk=$PWD/.local_install/dpdk/lib/x86_64-linux-gnu/pkgconfig
sed -i -e "s|^prefix=.*|prefix=\${pcfiledir}/../../..|" -e '/^Requires.private:/d' "$dpdk"/*.pc
export PKG_CONFIG_PATH=$PWD/build/meson-uninstalled:$dpdk CC=clang CXX=clang++

meson setup build -Denable_unit_tests=true -Denable_fuzzing=true
meson compile -C build usdt_provider_header
# The projects include <mtl/...>, the path of an installed MTL, and MTL is not
# installed here. mtl-uninstalled.pc gives each project -I for build, so
# build/mtl gives the headers in include that path. Without it, clang-tidy
# stops at the first <mtl/...>. The error is in a header, not in the checked
# source file, so cpp-linter does not report it, and the file passes unchecked.
ln -sfn ../include build/mtl
for project in app tests plugins ld_preload manager tests/tools/RxTxApp ecosystem/gstreamer_plugin \
	tests/tools/gstreamer_tools ecosystem/obs_mtl/linux-mtl tools/set_tai_offset; do
	meson setup "build/$(basename "$project")" "$project"
done
jq -s add build/compile_commands.json build/*/compile_commands.json >compile_commands.json
