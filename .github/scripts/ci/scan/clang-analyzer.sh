#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Usage: clang-analyzer.sh {analyze|check}; the ci:clang-analyzer tasks describe both.
set -euo pipefail

# CodeChecker runs the first clang on PATH; CTU needs its clang-extdef-mapping too.
export PATH=/usr/lib/llvm-18/bin:$PATH

case "${1:-}" in
analyze)
	jq '[.[] | select(.output | startswith("lib/libmtl.so.p/"))
		| .command |= gsub("-march=native"; "-march=x86-64-v4")]' \
		compile_commands.json >csa_compile_commands.json
	CodeChecker analyze csa_compile_commands.json --analyzers clangsa --ctu \
		--analyzer-config clangsa:ctu-phase1-inlining=all --disable deadcode.DeadStores \
		--skip .github/code-scan/codechecker.skip --drop-reports-from-skipped-files \
		-j "$(nproc)" -o csa-reports
	# parse exits 2 when there is a report; check decides on that.
	CodeChecker parse csa-reports -e sarif -o csa.sarif \
		--review-status unreviewed confirmed >/dev/null || [ $? -eq 2 ]
	;;
check)
	CodeChecker parse csa-reports --print-steps
	;;
*)
	echo "Usage: $0 {analyze|check}" >&2
	exit 2
	;;
esac
