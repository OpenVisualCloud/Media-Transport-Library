#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root_dir"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

hash_output=$(bash script/hash_sources.sh)
grep -q '^  jpegxs:' <<<"$hash_output" || fail "jpegxs source hash is missing"
grep -q '^  ice:' <<<"$hash_output" || fail "ice source hash is missing"

task_list=$(task --list-all)
for task_name in \
	ci:hash-dependencies \
	ci:build-dependencies \
	ci:validate-dependencies \
	ci:build-jpegxs \
	ci:validate-jpegxs \
	ci:build-ice \
	ci:validate-ice \
	ci:activate-ice \
	ci:validate-host; do
	grep -q "^\* ${task_name}:" <<<"$task_list" || fail "Taskfile task ${task_name} is missing"
done

for script in \
	.github/scripts/ci/build-jpegxs.sh \
	.github/scripts/ci/validate-jpegxs.sh \
	.github/scripts/ci/validate-cache.sh \
	.github/scripts/ci/validate-ice.sh \
	.github/scripts/ci/activate-ice.sh \
	.github/scripts/ci/configure-host.sh; do
	test -x "$script" || fail "$script is missing or not executable"
done

bash .github/scripts/ci/check-yaml-policy.sh

task_path_literal="TASK_BIN=\$(command -v task)"
grep -Fq "$task_path_literal" .github/actions/validate-host/action.yml ||
	fail "validate-host does not preserve the absolute Task binary path"
if grep -q 'sudo -E task ' .github/actions/validate-host/action.yml; then
	fail "validate-host resolves Task through sudo secure_path"
fi

if grep -Eq 'build_ice_driver|make install|modprobe|rmmod' .github/workflows/gtest-bare-metal.yml; then
	fail "gtest-bare-metal still builds or activates ICE"
fi

if grep -Eq 'sudo|modprobe|rmmod|depmod|make install' script/build_ice_driver.sh; then
	fail "ICE build-only script still mutates the host"
fi

echo "prebuilt dependency contracts: PASS"