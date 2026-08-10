#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "${script_dir}/cache-schema.env"

output=${GITHUB_OUTPUT:-/dev/stdout}
architecture=${ICE_ARCH:-$(uname -m)}
kernel_release=${ICE_KERNEL_RELEASE:-$(uname -r)}
schema=${CI_CACHE_SCHEMA:?CI_CACHE_SCHEMA is required}
if [ -n "${ICE_ABI_SHA256:-}" ]; then
	ice_abi_sha256=$ICE_ABI_SHA256
else
	abi_output=$(mktemp)
	trap 'rm -f "$abi_output"' EXIT
	GITHUB_OUTPUT="$abi_output" bash "${script_dir}/ice-abi.sh"
	ice_abi_sha256=$(sed -n 's/^abi_sha256=//p' "$abi_output")
fi
test -n "$ice_abi_sha256"

for component in dpdk mtl jpegxs ffmpeg gstreamer plugins ice; do
	upper=${component^^}
	hash_var="HASH_${upper}"
	value=${!hash_var:?${hash_var} is required}
	case "$component" in
	jpegxs) key="stash-v${schema}-jpegxs-${architecture}-${value}" ;;
	ice) key="stash-v${schema}-ice-${value}-${kernel_release}-${architecture}-${ice_abi_sha256}" ;;
	*) key="stash-v${schema}-${component}-${value}" ;;
	esac
	printf '%s=%s\n' "${component}_key" "$key" >>"$output"
done
{
	echo "kernel_release=${kernel_release}"
	echo "architecture=${architecture}"
	echo "cache_schema=${schema}"
} >>"$output"