#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Build prerequisites for the unit tier, which needs no NIC and no root.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

# No sudo: the script sudos where it needs root, keeping the cache tree runner-owned.
bash "${root_dir}/.github/scripts/setup_environment.sh"
