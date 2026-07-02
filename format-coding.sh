#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Format all code: C/C++ (clang-format), Python (isort/black),
# Markdown (markdownlint), Shell (shfmt).
#
# Tool versions are pinned below. A different local version can format
# differently and silently churn hundreds of unrelated files — every
# formatter is version-checked before it runs; a mismatch aborts the
# script instead of reformatting with the wrong ruleset.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd "$SCRIPT_DIR" >/dev/null

command_exists() { command -v "$1" >/dev/null 2>&1; }

# Pinned tool versions — bump deliberately, together with a full-tree
# reformat commit, never as a side effect of "whatever is installed".
CLANG_FORMAT_MAJOR_VER="14"
BLACK_PIN_VER="24.4.0"
ISORT_PIN_VER="5.13.2"
SHFMT_PIN_VER="3.8.0"

# Aborts with a clear message if $actual != $required. $install_hint is
# printed as the remediation command.
require_version() {
	local tool=$1 required=$2 actual=$3 install_hint=$4
	if [[ "$actual" != "$required" ]]; then
		echo "ERROR: $tool version mismatch (found '$actual', need '$required')." >&2
		echo "A wrong version reformats unrelated files with a huge, unreviewable diff." >&2
		echo "Install the pinned version: $install_hint" >&2
		return 1
	fi
}

# ==========================
# C/C++ — clang-format
# ==========================
CLANG_FORMAT_TOOL=${CLANG_FORMAT_TOOL:-clang-format-14}
echo "=== clang-format ==="

if command_exists "$CLANG_FORMAT_TOOL"; then
	CLANG_FORMAT_ACTUAL=$("$CLANG_FORMAT_TOOL" --version | grep -oP '\d+\.\d+\.\d+' | head -1)
	require_version "$CLANG_FORMAT_TOOL" "$CLANG_FORMAT_MAJOR_VER" \
		"${CLANG_FORMAT_ACTUAL%%.*}" "sudo apt install ${CLANG_FORMAT_TOOL}"
else
	echo "ERROR: $CLANG_FORMAT_TOOL not found. Install: sudo apt install ${CLANG_FORMAT_TOOL}" >&2
	exit 1
fi

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
	BLACK_ACTUAL=$(black --version | head -1 | grep -oP '\d+\.\d+\.\d+')
	ISORT_ACTUAL=$(isort --version-number)
	require_version "black" "$BLACK_PIN_VER" "$BLACK_ACTUAL" "pip install 'black==${BLACK_PIN_VER}'"
	require_version "isort" "$ISORT_PIN_VER" "$ISORT_ACTUAL" "pip install 'isort==${ISORT_PIN_VER}'"
	isort python/ tests/
	black python/ tests/
else
	echo "isort/black not found — skipping Python formatting."
	echo "please use: pip install 'black==${BLACK_PIN_VER}' 'isort==${ISORT_PIN_VER}'"
fi

# ==========================
# Markdown — markdownlint
# ==========================
echo ""
echo "=== markdownlint ==="

mapfile -t MD_FILES < <(git ls-files '*.md')
if ((${#MD_FILES[@]})); then
	if command_exists npx; then
		if ! npx --yes markdownlint-cli@0.43.0 \
			--config "$SCRIPT_DIR/.github/linters/.markdown-lint.yml" \
			--fix "${MD_FILES[@]}"; then
			echo ""
			echo "markdownlint found issues --fix cannot auto-correct (e.g. MD013"
			echo "line-length). Fix them by hand, then re-run ./format-coding.sh."
			exit 1
		fi
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
		SHFMT_ACTUAL=$(shfmt --version | grep -oP '\d+\.\d+\.\d+')
		require_version "shfmt" "$SHFMT_PIN_VER" "$SHFMT_ACTUAL" \
			"see https://github.com/mvdan/sh/releases/tag/v${SHFMT_PIN_VER}"
		shfmt -w "${SH_FILES[@]}"
		echo "Formatted ${#SH_FILES[@]} shell scripts."
	else
		echo "shfmt not found — skipping. Install with: sudo apt-get install shfmt"
	fi
fi

popd >/dev/null
echo ""
echo "Done."
