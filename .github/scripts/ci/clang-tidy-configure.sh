#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Configures the meson projects for code-scan.yml's clang-tidy job, which needs
# their compile commands, not their binaries: libmtl, also as the unit suite
# and the fuzz harnesses, and each project that Base Build compiles against it.
# DPDK and MTL are the Build workflow's, restored to .local_install. The only
# compile is gpu_direct's, since the root project finds it through pkg-config
# once it is installed, and the only generated file is the mt_usdt_provider.h
# that libmtl's sources include.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
local_install="${root_dir}/.local_install"
cd "$root_dir"

# The caches come from the self-hosted build runner, so their .pc files name
# its prefix. Make each prefix relative to the .pc file, as build-jpegxs.sh
# does: <prefix>/lib/x86_64-linux-gnu/pkgconfig is three levels down. They
# also name, as Requires.private for a static link, the driver libraries that
# DPDK found there (libmlx5, libxdp...), which this runner lacks; compile
# commands link nothing, so drop them.
pkg_dirs=("${local_install}/dpdk/lib/x86_64-linux-gnu/pkgconfig" "${local_install}/mtl/lib/x86_64-linux-gnu/pkgconfig")
sed -i -e "s|^prefix=.*|prefix=\${pcfiledir}/../../..|" -e '/^Requires.private:/d' "${pkg_dirs[0]}"/*.pc "${pkg_dirs[1]}"/*.pc
export PKG_CONFIG_PATH="${pkg_dirs[0]}:${pkg_dirs[1]}"

export LIBRARY_PATH="/usr/local/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
meson setup gpu_direct/build gpu_direct
meson compile -C gpu_direct/build
sudo meson install -C gpu_direct/build --no-rebuild

meson setup build -Dbuildtype=release
meson setup build_unit -Dbuildtype=release -Denable_unit_tests=true
CC=clang CXX=clang++ meson setup build_fuzz -Dbuildtype=release -Denable_fuzzing=true
for dir in build build_unit build_fuzz; do
	meson compile -C "$dir" usdt_provider_header
done

for project in app tests plugins ld_preload manager tests/tools/RxTxApp \
	ecosystem/gstreamer_plugin tests/tools/gstreamer_tools \
	ecosystem/obs_mtl/linux-mtl tools/set_tai_offset; do
	meson setup "build/$(basename "$project")" "$project" -Dbuildtype=release
done
