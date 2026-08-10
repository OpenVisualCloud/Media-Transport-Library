#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

compiler=${1:-${CC:-cc}}
{
	"$compiler" -dumpmachine
	"$compiler" -dumpfullversion -dumpversion
} | sha256sum | cut -d' ' -f1