#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Format all code: C/C++ (clang-format), Python (isort/black),
# Markdown (markdownlint), Markdown prose (textlint), and Shell
# (shfmt, shellcheck).
#
# Tool versions are pinned below. A different local version can format
# differently and silently churn hundreds of unrelated files — every
# formatter is version-checked before it runs; a mismatch aborts the
# script instead of reformatting with the wrong ruleset.
#
# Pass --check to preview: every formatter runs in its non-mutating
# check/diff mode instead of writing to disk, and the script exits 1 if
# anything would change. Use this to safely inspect a formatting change's
# real blast radius before ever letting it touch a real working tree.
#
# Adding a new tool: write a run_<tool>() function following the shape of
# the existing ones, then add a call to it in main() at the bottom.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command_exists() { command -v "$1" >/dev/null 2>&1; }

CHECK_MODE=0
[[ "${1:-}" == "--check" ]] && CHECK_MODE=1
NEEDS_CHANGES=0

# Pinned tool versions — bump deliberately, together with a full-tree
# reformat commit, never as a side effect of "whatever is installed".
CLANG_FORMAT_MAJOR_VER="14"
BLACK_PIN_VER="24.4.0"
ISORT_PIN_VER="5.13.2"
SHFMT_PIN_VER="3.8.0"
SHELLCHECK_PIN_VER="0.10.0"
TEXTLINT_PIN_VER="14.0.4"
TEXTLINT_TERMINOLOGY_PIN_VER="4.0.1"
TEXTLINT_COMMENTS_PIN_VER="1.2.2"

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

# Call right after running a tool that has no (or incomplete) autofix,
# with its exit code as $1. In --check mode this just flags NEEDS_CHANGES
# so the run summarizes at the end. In write mode, $2 (if given) is
# printed and the script hard-exits — the tool couldn't fix everything
# itself, so the rest of the diff needs a human. Formatters that always
# succeed once applied (clang-format -i, isort, black, shfmt -w) don't
# need this at all in their write-mode branch.
note_result() {
	local rc=$1 unfixable_msg=${2:-}
	((rc == 0)) && return 0
	if ((CHECK_MODE)); then
		NEEDS_CHANGES=1
		return 0
	fi
	if [[ -n "$unfixable_msg" ]]; then
		echo "" >&2
		echo "$unfixable_msg" >&2
		echo "Fix by hand, then re-run ./format-coding.sh." >&2
		exit 1
	fi
}

section() {
	echo ""
	echo "=== $1 ==="
}

# ==========================
# C/C++ — clang-format
# ==========================
run_clang_format() {
	section "clang-format"
	local tool=${CLANG_FORMAT_TOOL:-clang-format-14}

	if command_exists "$tool"; then
		local actual
		actual=$("$tool" --version | grep -oP '\d+\.\d+\.\d+' | head -1)
		require_version "$tool" "$CLANG_FORMAT_MAJOR_VER" "${actual%%.*}" "sudo apt install ${tool}"
	else
		echo "ERROR: $tool not found. Install: sudo apt install ${tool}" >&2
		exit 1
	fi

	local files
	mapfile -t files < <(
		git ls-files --cached --others --exclude-standard \
			-- '*.cpp' '*.hpp' '*.cc' '*.c' '*.h' \
			':!:pymtl_wrap.c' ':!:vmlinux.h'
	)
	((${#files[@]})) || return 0

	if ((CHECK_MODE)); then
		printf '%s\0' "${files[@]}" | xargs -0 -n32 -P"$(nproc)" "$tool" --dry-run --Werror ||
			note_result $?
		echo "Checked ${#files[@]} C/C++ files."
	else
		printf '%s\0' "${files[@]}" | xargs -0 -n32 -P"$(nproc)" "$tool" -i
		echo "Formatted ${#files[@]} C/C++ files."
	fi
}

# ==========================
# Python — isort + black
# ==========================
run_python() {
	section "Python (isort + black)"

	local files
	mapfile -t files < <(git ls-files '*.py')
	((${#files[@]})) || return 0

	if ! command_exists isort || ! command_exists black; then
		echo "isort/black not found — skipping Python formatting."
		echo "please use: pip install 'black==${BLACK_PIN_VER}' 'isort==${ISORT_PIN_VER}'"
		return 0
	fi

	local black_actual isort_actual
	black_actual=$(black --version | head -1 | grep -oP '\d+\.\d+\.\d+')
	isort_actual=$(isort --version-number)
	require_version "black" "$BLACK_PIN_VER" "$black_actual" "pip install 'black==${BLACK_PIN_VER}'"
	require_version "isort" "$ISORT_PIN_VER" "$isort_actual" "pip install 'isort==${ISORT_PIN_VER}'"

	# --profile black: isort's own default import-wrapping style disagrees
	# with black's, so without this every already-black-clean file gets
	# printed as "Fixing" by isort and then immediately "reformatted" back
	# by black — a no-op net change reported as if it were real, confusing
	# the log. This profile makes isort agree with black up front.
	#
	# All git-tracked *.py files, not just python/ tests/ — CI's Super-Linter
	# scans the whole tree (e.g. .github/mcp/*.py), so a narrower scope here
	# silently misses files that still fail in CI.
	if ((CHECK_MODE)); then
		isort --profile black --check-only --diff "${files[@]}" || note_result $?
		black --check --diff "${files[@]}" || note_result $?
	else
		isort --profile black "${files[@]}"
		black "${files[@]}"
	fi
}

# ==========================
# Markdown — markdownlint
# ==========================
run_markdownlint() {
	section "markdownlint"

	mapfile -t MD_FILES < <(git ls-files '*.md')
	((${#MD_FILES[@]})) || return 0

	if ! command_exists npx; then
		echo "npx not found — skipping. Install with: sudo apt-get install npm"
		return 0
	fi

	local args=(--config "$SCRIPT_DIR/.github/linters/.markdown-lint.yml")
	((CHECK_MODE)) || args+=(--fix)
	npx --yes markdownlint-cli@0.43.0 "${args[@]}" "${MD_FILES[@]}" ||
		note_result $? "markdownlint found issues --fix cannot auto-correct (e.g. MD013 line-length)."
}

# ==========================
# Markdown prose — textlint (terminology)
# ==========================
# CI's VALIDATE_NATURAL_LANGUAGE checks prose terminology (e.g. "repo" ->
# "repository") that markdownlint doesn't cover — it only lints Markdown
# syntax, not word choice. Config mirrors Super-Linter's default
# TEMPLATES/.textlintrc (terminology rule + comments filter). Depends on
# MD_FILES from run_markdownlint, so must run after it.
run_textlint() {
	section "textlint (terminology)"

	((${#MD_FILES[@]})) || return 0

	if ! command_exists npx; then
		echo "npx not found — skipping. Install with: sudo apt-get install npm"
		return 0
	fi

	local args=(--config "$SCRIPT_DIR/.github/linters/.textlintrc")
	((CHECK_MODE)) || args+=(--fix)
	npx --yes \
		-p "textlint@${TEXTLINT_PIN_VER}" \
		-p "textlint-rule-terminology@${TEXTLINT_TERMINOLOGY_PIN_VER}" \
		-p "textlint-filter-rule-comments@${TEXTLINT_COMMENTS_PIN_VER}" \
		textlint "${args[@]}" "${MD_FILES[@]}" ||
		note_result $? "textlint found terminology issues --fix could not auto-correct."
}

# ==========================
# Shell — shfmt
# ==========================
run_shfmt() {
	section "shfmt"

	mapfile -t SH_FILES < <(git ls-files '*.sh')
	((${#SH_FILES[@]})) || return 0

	if ! command_exists shfmt; then
		echo "shfmt not found — skipping. Install with: sudo apt-get install shfmt"
		return 0
	fi

	local actual
	actual=$(shfmt --version | grep -oP '\d+\.\d+\.\d+')
	require_version "shfmt" "$SHFMT_PIN_VER" "$actual" \
		"see https://github.com/mvdan/sh/releases/tag/v${SHFMT_PIN_VER}"

	if ((CHECK_MODE)); then
		shfmt -d "${SH_FILES[@]}" || note_result $?
		echo "Checked ${#SH_FILES[@]} shell scripts."
	else
		shfmt -w "${SH_FILES[@]}"
		echo "Formatted ${#SH_FILES[@]} shell scripts."
	fi
}

# ==========================
# Shell — shellcheck
# ==========================
# shfmt above only reformats whitespace/style — it never runs shellcheck's
# actual static analysis (unreachable code, unused vars, quoting bugs), so
# it silently misses everything CI's VALIDATE_BASH catches. There's no
# --fix mode: issues must be fixed by hand.
#
# All shell files are passed to a single shellcheck invocation (not one
# process per file) so `source`d sibling scripts resolve and don't spuriously
# trip SC1091. Depends on SH_FILES from run_shfmt, so must run after it.
run_shellcheck() {
	section "shellcheck"

	((${#SH_FILES[@]})) || return 0

	if ! command_exists shellcheck; then
		echo "shellcheck not found — skipping. Install with: pip install 'shellcheck-py==${SHELLCHECK_PIN_VER}.1'"
		return 0
	fi

	local actual
	actual=$(shellcheck --version | grep -oP '(?<=version: )\d+\.\d+\.\d+')
	require_version "shellcheck" "$SHELLCHECK_PIN_VER" "$actual" \
		"pip install 'shellcheck-py==${SHELLCHECK_PIN_VER}.1'"

	shellcheck "${SH_FILES[@]}" || note_result $? "shellcheck found issues it cannot auto-fix."
	echo "Checked ${#SH_FILES[@]} shell scripts."
}

main() {
	pushd "$SCRIPT_DIR" >/dev/null

	run_clang_format
	run_python
	run_markdownlint
	run_textlint
	run_shfmt
	run_shellcheck

	popd >/dev/null
	echo ""
	if ((CHECK_MODE)); then
		if ((NEEDS_CHANGES)); then
			echo "Some files would be reformatted — run ./format-coding.sh (without --check) to fix."
			exit 1
		fi
		echo "Everything already formatted."
	else
		echo "Done."
	fi
}

main
