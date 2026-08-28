#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "${script_dir}/cache-schema.env"

output=${GITHUB_OUTPUT:-/dev/stdout}
architecture=$(uname -m)
kernel_release=$(uname -r)
schema=${CI_CACHE_SCHEMA:?CI_CACHE_SCHEMA is required}
compiler_sha256=$(bash "${script_dir}/compiler-identity.sh" producer)
jpegxs_compiler_sha256=${JPEGXS_COMPILER_SHA256:-$compiler_sha256}

for component in dpdk mtl jpegxs ffmpeg gstreamer plugins ice; do
	upper=${component^^}
	hash_var="HASH_${upper}"
	value=${!hash_var:?${hash_var} is required}
	case "$component" in
	jpegxs) key="stash-v${schema}-jpegxs-${architecture}-${jpegxs_compiler_sha256}-${value}" ;;
	# A .ko only loads into the kernel it was built for, so the ICE bundle is
	# per kernel release as well as per patch hash. Nothing finer: validate-ice.sh
	# reads the vermagic of the file that arrives, which is what actually decides
	# whether the module loads here.
	ice) key="stash-v${schema}-ice-${value}-${kernel_release}-${architecture}" ;;
	*) key="stash-v${schema}-${component}-${value}" ;;
	esac
	printf '%s=%s\n' "${component}_key" "$key" >>"$output"
done
{
	echo "kernel_release=${kernel_release}"
	echo "architecture=${architecture}"
	echo "cache_schema=${schema}"
	echo "jpegxs_compiler_sha256=${jpegxs_compiler_sha256}"
} >>"$output"
