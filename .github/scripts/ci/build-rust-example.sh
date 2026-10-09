#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# HOOK_RUST builds rust/, but cargo skips examples, so this bindgen struct
# literal over mtl_init_params never sees a compiler. Only this one example: the
# imtl-rs examples need libsdl2-dev. No ancestor manifest declares [workspace],
# so imtl-sys resolves its own lock and needs the same `home` pin HOOK_RUST
# applies -- apt rustc cannot build current `home`. sudo -E matches HOOK_RUST,
# whose cargo populated the ~/.cargo registry as root.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck source=/dev/null
. "${root_dir}/versions.env"

cd "${root_dir}/rust/imtl-sys"
sudo -E cargo update home --precise "${RUST_HOOK_CARGO_VER:?}"
sudo -E cargo build --example no_std
