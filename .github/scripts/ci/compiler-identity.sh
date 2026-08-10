#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

compiler=${1:-${CC:-cc}}
if [ "$compiler" = producer ]; then
	script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
	# shellcheck disable=SC1091
	. "${script_dir}/cache-schema.env"
	printf '%s\n%s\n' "$CI_PRODUCER_COMPILER_TARGET" "$CI_PRODUCER_COMPILER_VERSION" |
		sha256sum | cut -d' ' -f1
	exit 0
fi
{
	"$compiler" -dumpmachine
	"$compiler" -dumpfullversion -dumpversion
} | sha256sum | cut -d' ' -f1