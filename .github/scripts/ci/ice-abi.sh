#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

kernel_release=${ICE_KERNEL_RELEASE:-$(uname -r)}
architecture=${ICE_ARCH:-$(uname -m)}
kernel_build=${ICE_KERNEL_BUILD_DIR:-/lib/modules/${kernel_release}/build}
output=${GITHUB_OUTPUT:-/dev/stdout}

test -d "$kernel_build" || {
	echo "kernel build tree is missing: ${kernel_build}" >&2
	exit 1
}

case "$architecture" in
x86_64) kernel_arch=x86 ;;
aarch64) kernel_arch=arm64 ;;
*) kernel_arch=$architecture ;;
esac

inputs=$(mktemp)
trap 'rm -f "$inputs"' EXIT
for path in \
	.config Module.symvers Makefile \
	include/config/auto.conf include/generated/autoconf.h \
	include/generated/compile.h include/generated/utsrelease.h; do
	[ -f "${kernel_build}/${path}" ] && printf '%s\0' "$path" >>"$inputs"
done
for directory in include/config include/generated "arch/${kernel_arch}/include/generated"; do
	[ -d "${kernel_build}/${directory}" ] || continue
	while IFS= read -r -d '' relative_path; do
		printf '%s/%s\0' "$directory" "$relative_path" >>"$inputs"
	done < <(find "${kernel_build}/${directory}" -type f -printf '%P\0')
done

test -s "$inputs" || {
	echo "kernel ABI inputs are missing from ${kernel_build}" >&2
	exit 1
}

abi_sha256=$(
	cd "$kernel_build"
	LC_ALL=C sort -zu "$inputs" | xargs -0 sha256sum | sha256sum | cut -d' ' -f1
)
compile_header="${kernel_build}/include/generated/compile.h"
if [ -f "$compile_header" ]; then
	kernel_compiler_sha256=$(sha256sum "$compile_header" | cut -d' ' -f1)
else
	kernel_compiler_sha256=unknown
fi

echo "abi_sha256=${abi_sha256}" >>"$output"
echo "kernel_compiler_sha256=${kernel_compiler_sha256}" >>"$output"