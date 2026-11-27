#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Thin CI wrapper invoking tests/integration_tests/runner/gtest_runner.py.

set -euo pipefail

mtl_folder="$(realpath "$(dirname "${BASH_SOURCE[0]}")/../..")"
exec python3 "${mtl_folder}/tests/integration_tests/runner/gtest_runner.py" "$@"
