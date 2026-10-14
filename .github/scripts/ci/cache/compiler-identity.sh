#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

if [ "${1:-}" = producer ]; then
	printf 'x86_64-linux-gnu\n13.3.0\n'
else
	"${CC:-cc}" -dumpmachine
	"${CC:-cc}" -dumpfullversion -dumpversion
fi | sha256sum | cut -d' ' -f1
