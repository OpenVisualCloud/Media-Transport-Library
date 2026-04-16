#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Format all code: C/C++ (clang-format), Python (isort/black),
# Markdown (markdownlint), Shell (shfmt).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd "$SCRIPT_DIR" >/dev/null

command_exists() { command -v "$1" >/dev/null 2>&1; }

# ==========================
# C/C++ — clang-format
# ==========================
CLANG_FORMAT_TOOL=${CLANG_FORMAT_TOOL:-clang-format-14}
echo "=== clang-format ==="

mapfile -t CLANG_FORMAT_FILES < <(
	git ls-files --cached --others --exclude-standard \
		-- '*.cpp' '*.hpp' '*.cc' '*.c' '*.h' \
		':!:pymtl_wrap.c' ':!:vmlinux.h'
)

if ((${#CLANG_FORMAT_FILES[@]})); then
	printf '%s\0' "${CLANG_FORMAT_FILES[@]}" |
		xargs -0 -n32 -P"$(nproc)" "${CLANG_FORMAT_TOOL}" -i
	echo "Formatted ${#CLANG_FORMAT_FILES[@]} C/C++ files."
fi

# ==========================
# Python — isort + black
# ==========================
echo ""
echo "=== Python (isort + black) ==="

if command_exists isort && command_exists black; then
	isort python/ tests/
	black python/ tests/
else
	echo "isort/black not found — skipping Python formatting."
	echo "please use: pip install 'black==24.4.0' 'isort==5.13.2'"
fi

# ==========================
# Markdown — markdownlint
# ==========================
echo ""
echo "=== markdownlint ==="

mapfile -t MD_FILES < <(git ls-files '*.md')
if ((${#MD_FILES[@]})); then
	if command_exists npx; then
		npx --yes markdownlint-cli@0.43.0 \
			--config "$SCRIPT_DIR/.github/linters/.markdown-lint.yml" \
			--fix "${MD_FILES[@]}" || true
	else
		echo "npx not found — skipping. Install with: sudo apt-get install npm"
	fi
fi

# ==========================
# Shell — shfmt
# ==========================
echo ""
echo "=== shfmt ==="

mapfile -t SH_FILES < <(git ls-files '*.sh')
if ((${#SH_FILES[@]})); then
	if command_exists shfmt; then
		shfmt -w "${SH_FILES[@]}"
		echo "Formatted ${#SH_FILES[@]} shell scripts."
	else
		echo "shfmt not found — skipping. Install with: sudo apt-get install shfmt"
	fi
fi

popd >/dev/null
echo ""
echo "Done."
