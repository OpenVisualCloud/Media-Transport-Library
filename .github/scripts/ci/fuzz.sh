#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Usage: fuzz.sh {build|run}, from the repo root. FUZZ_TIME = seconds per target.
set -euo pipefail

build_dir=${FUZZ_BUILD_DIR:-build_fuzz}
fuzz_time=${FUZZ_TIME:-600}

case "${1:-}" in
build)
	# clang has no gcc libasan, so ASan comes from b_sanitize, not enable_asan.
	reconfigure=()
	[[ -d $build_dir ]] && reconfigure=(--reconfigure)
	CC=clang CXX=clang++ meson setup "${reconfigure[@]}" "$build_dir" . \
		-Denable_fuzzing=true -Db_sanitize=address -Db_lundef=false \
		-Dc_args=-Wno-error=unused-but-set-variable
	ninja -C "$build_dir"
	;;
run)
	mkdir -p fuzz-report
	status=0
	for binary in "$build_dir"/tests/fuzz/*_fuzz; do
		name=$(basename "$binary")
		mkdir -p "fuzz-corpus/$name" "fuzz-report/$name"
		"$binary" -max_total_time="$fuzz_time" -timeout=60 -artifact_prefix="fuzz-report/$name/" \
			"fuzz-corpus/$name" >"fuzz-report/$name.log" 2>&1 || {
			echo "::error::$name failed, see fuzz-report/$name.log"
			tail -n 50 "fuzz-report/$name.log"
			status=1
		}
	done
	exit "$status"
	;;
*)
	echo "Usage: $0 {build|run}" >&2
	exit 2
	;;
esac
