#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"

kernel_release=${ICE_KERNEL_RELEASE:-$(uname -r)}
architecture=${ICE_ARCH:-$(uname -m)}
if [ "$kernel_release" != "$(uname -r)" ] || [ "$architecture" != "$(uname -m)" ]; then
	echo "ICE can only be built for the current kernel and architecture" >&2
	exit 1
fi

bundle_root=${ICE_BUNDLE_ROOT:-"${root_dir}/.local_install/ice"}
output_dir=${ICE_OUTPUT_DIR:-"${bundle_root}/${kernel_release}/${architecture}"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/mtl-ice-build.XXXXXX")
archive="${work_dir}/ice-${ICE_VER}.tar.gz"
stage_root="${bundle_root}.tmp.$$"
stage="${stage_root}/${kernel_release}/${architecture}"
trap 'rm -rf "$work_dir" "$stage_root"' EXIT

test -d "/lib/modules/${kernel_release}/build" || {
	echo "kernel headers are missing for ${kernel_release}" >&2
	exit 1
}

build_compiler=${CC:-cc}
command -v "$build_compiler" >/dev/null 2>&1 || {
	echo "ICE build compiler is unavailable: ${build_compiler}" >&2
	exit 1
}
kernel_compile_header="/lib/modules/${kernel_release}/build/include/generated/compile.h"
if [ -f "$kernel_compile_header" ] && grep -qi 'gcc' "$kernel_compile_header"; then
	kernel_gcc_major=$(grep -oEi 'gcc[^0-9]*[0-9]+' "$kernel_compile_header" | grep -oE '[0-9]+' | head -n1 || true)
	build_gcc_major=$($build_compiler -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1 || true)
	if [ -n "$kernel_gcc_major" ] && [ "$build_gcc_major" != "$kernel_gcc_major" ]; then
		echo "ICE build requires GCC ${kernel_gcc_major} for ${kernel_release}; ${build_compiler} reports ${build_gcc_major:-unknown}" >&2
		exit 1
	fi
fi

if ! curl --fail --location --connect-timeout 15 --max-time 30 "$ICE_REPO" --output "$archive"; then
	curl --fail --location --retry 3 --connect-timeout 15 --max-time 180 \
		"https://github.com/intel/ethernet-linux-ice/archive/refs/tags/v${ICE_VER}.tar.gz" \
		--output "$archive"
fi
tar -xzf "$archive" -C "$work_dir"
source_dir=$(find "$work_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)
test -n "$source_dir"

for patch_file in "${root_dir}"/patches/ice_drv/"${ICE_VER}"/*.patch; do
	patch -d "$source_dir" -p1 -i "$patch_file"
done
make -C "${source_dir}/src" -j"$(nproc)" CC="$build_compiler"

module=$(find "${source_dir}/src" -name ice.ko -type f -print -quit)
test -n "$module"
mkdir -p "$stage"
install -m 0644 "$module" "${stage}/ice.ko"

hash_output=$(mktemp)
bash "${root_dir}/script/hash_sources.sh" -o "$hash_output" >/dev/null
source_hash=$(sed -n 's/^ice=//p' "$hash_output")
rm -f "$hash_output"
module_hash=$(sha256sum "${stage}/ice.ko" | cut -d' ' -f1)
compiler_hash=$(bash "${root_dir}/.github/scripts/ci/compiler-identity.sh" "$build_compiler")
abi_output=$(mktemp)
GITHUB_OUTPUT="$abi_output" bash "${root_dir}/.github/scripts/ci/ice-abi.sh"
kernel_abi_hash=$(sed -n 's/^abi_sha256=//p' "$abi_output")
kernel_compiler_hash=$(sed -n 's/^kernel_compiler_sha256=//p' "$abi_output")
rm -f "$abi_output"

cat >"${stage}/metadata.env" <<EOF
schema=2
source_hash=${source_hash}
ice_version=${ICE_VER}
ice_dmid=${ICE_DMID}
kernel_release=${kernel_release}
architecture=${architecture}
compiler_sha256=${compiler_hash}
kernel_compiler_sha256=${kernel_compiler_hash}
kernel_abi_sha256=${kernel_abi_hash}
vermagic=$(modinfo -F vermagic "${stage}/ice.ko")
module_sha256=${module_hash}
signer=$(modinfo -F signer "${stage}/ice.ko")
sig_id=$(modinfo -F sig_id "${stage}/ice.ko")
EOF

ICE_BUNDLE_ROOT="$stage_root" \
	ICE_KERNEL_RELEASE="$kernel_release" ICE_ARCH="$architecture" \
	bash "${root_dir}/.github/scripts/ci/validate-ice.sh"
rm -rf "$output_dir"
mkdir -p "$(dirname "$output_dir")"
mv "$stage" "$output_dir"
rm -rf "$stage_root"
echo "ICE artifact built at ${output_dir}"
