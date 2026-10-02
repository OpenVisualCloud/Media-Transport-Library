#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

# Build the libFuzzer harnesses of tests/fuzz with clang and AddressSanitizer,
# and run each target for FUZZ_TIME seconds. The targets run one after the
# other: the RX harnesses start their EAL with -c1, so two of them at the same
# time share CPU 0.
#
# Usage: fuzz.sh {build|run}
#
#   FUZZ_BUILD_DIR   build directory (default: build_fuzz)
#   FUZZ_REPORT_DIR  summary.md, logs/, artifacts/, repro/ (default: fuzz-report)
#   FUZZ_CORPUS_DIR  one corpus directory per target (default: fuzz-corpus)
#   FUZZ_TIME        seconds per target (default: 600)
#
# `run` fails when a target exits with a code that is not zero, or writes a
# crash-, leak- or timeout- file. It runs each such file again into repro/, to
# get one stack trace per input.

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
build_dir=${FUZZ_BUILD_DIR:-${root_dir}/build_fuzz}
report_dir=${FUZZ_REPORT_DIR:-${root_dir}/fuzz-report}
corpus_dir=${FUZZ_CORPUS_DIR:-${root_dir}/fuzz-corpus}
fuzz_time=${FUZZ_TIME:-600}

# The last number that matches the pattern $1 in the file $2, or nothing.
last_number() {
	grep -oE "$1" "$2" | tail -n 1 | grep -oE '[0-9]+$' || true
}

summary() {
	local binary name log rc status_line cov ft corp faults finding
	echo "# Fuzz report"
	echo
	echo "- Commit: $(git -C "$root_dir" log -1 --format='%h %s' 2>/dev/null || echo unknown)"
	echo "- Compiler: $(clang --version | head -n 1)"
	echo "- Time per target: ${fuzz_time} s"
	echo
	echo "| Target | Exit | Runs | exec/s | cov | ft | Corpus | Faults | Finding |"
	echo "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |"
	for binary in "${targets[@]}"; do
		name=$(basename "$binary")
		log=${report_dir}/logs/${name}.log
		rc=$(cat "${report_dir}/logs/${name}.exit")
		status_line=$(grep -E '^#[0-9]+' "$log" | tail -n 1 || true)
		cov=$(grep -oE 'cov: [0-9]+' <<<"$status_line" | cut -d' ' -f2 || true)
		ft=$(grep -oE 'ft: [0-9]+' <<<"$status_line" | cut -d' ' -f2 || true)
		corp=$(grep -oE 'corp: [^ ]+' <<<"$status_line" | cut -d' ' -f2 || true)
		faults=$(find "${report_dir}/artifacts/${name}" -type f | wc -l)
		finding=$(grep -m 1 -oE 'ERROR: [A-Za-z]+Sanitizer: [^(|]*|ERROR: libFuzzer: [a-z-]+' \
			"$log" | sed -E 's/ (on|at) (unknown )?(address|pc) .*//; s/ *$//' || true)
		echo "| ${name} | ${rc} | $(last_number 'stat::number_of_executed_units: *[0-9]+' "$log") |" \
			"$(last_number 'stat::average_exec_per_sec: *[0-9]+' "$log") | ${cov} | ${ft} |" \
			"${corp} | ${faults} | ${finding:--} |"
	done
}

case "${1:-}" in
build)
	# -Denable_asan=true looks for the gcc libasan, which clang does not supply,
	# so ASan comes from the meson b_sanitize option. The -Wno-error flag stops
	# the clang warning for the unused 'sent' counter of st_video_transmitter.c.
	reconfigure=()
	[[ -f ${build_dir}/build.ninja ]] && reconfigure=(--reconfigure)
	CC=clang CXX=clang++ meson setup "${reconfigure[@]}" "$build_dir" "$root_dir" \
		-Dbuildtype=debugoptimized -Denable_fuzzing=true -Denable_asan=false \
		-Db_sanitize=address -Db_lundef=false \
		-Dc_args=-Wno-error=unused-but-set-variable
	ninja -C "$build_dir"
	;;
run)
	# The executables that tests/fuzz/meson.build defines, from the build itself.
	mapfile -t targets < <(meson introspect --targets "$build_dir" | python3 -c '
import json, sys
for target in json.load(sys.stdin):
    if target["type"] == "executable" and target["defined_in"].endswith("/tests/fuzz/meson.build"):
        print(target["filename"][0])
' | sort)
	if [[ ${#targets[@]} -eq 0 ]]; then
		echo "::error::${build_dir} has no fuzz target. Run: $0 build" >&2
		exit 1
	fi
	mkdir -p "${report_dir}/logs" "${report_dir}/repro"
	export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:symbolize=1:detect_leaks=1}
	export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:symbolize=1}
	status=0
	for binary in "${targets[@]}"; do
		name=$(basename "$binary")
		mkdir -p "${corpus_dir}/${name}" "${report_dir}/artifacts/${name}"
		echo "::group::${name}: ${fuzz_time} s"
		# -timeout makes a hang a timeout- file. The outer timeout stops a
		# target that libFuzzer itself cannot stop, so the summary still comes.
		rc=0
		timeout --kill-after=30 "$((fuzz_time + 300))" "$binary" \
			-max_total_time="$fuzz_time" -timeout=60 -print_final_stats=1 \
			-artifact_prefix="${report_dir}/artifacts/${name}/" "${corpus_dir}/${name}" \
			>"${report_dir}/logs/${name}.log" 2>&1 || rc=$?
		echo "$rc" >"${report_dir}/logs/${name}.exit"
		tail -n 20 "${report_dir}/logs/${name}.log"
		echo "::endgroup::"
		faults=0
		for artifact in "${report_dir}/artifacts/${name}"/*; do
			[[ -f $artifact ]] || continue
			faults=$((faults + 1))
			"$binary" "$artifact" >"${report_dir}/repro/${name}-$(basename "$artifact").log" 2>&1 || true
		done
		if [[ $rc -ne 0 || $faults -ne 0 ]]; then
			echo "::error::${name} exited with code ${rc} and wrote ${faults} fault input(s), see logs/${name}.log"
			status=1
		fi
	done
	summary >"${report_dir}/summary.md"
	cat "${report_dir}/summary.md"
	if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
		cat "${report_dir}/summary.md" >>"${GITHUB_STEP_SUMMARY}"
	fi
	exit "$status"
	;;
*)
	echo "Usage: $0 {build|run}" >&2
	exit 2
	;;
esac
