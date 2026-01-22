#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Based on clang-format
# For ubuntu, pls "apt-get install clang-format"
# When updating clang format version, remeber to update also GHA clang lister version:
#  - uses: DoozyX/clang-format-lint-action@v0.18.2
#     with:
#       clangFormatVersion: '14'
#       source: '.'
#       extensions: 'hpp,h,cpp,c,cc'
# in
# .github/workflows/afxdp_build.yml
# .github/workflows/centos_build.yml
# .github/workflows/tools_build.yml
# .github/workflows/ubuntu_build.yml

set -euo pipefail

CLANG_FORMAT_TOOL=${CLANG_FORMAT_TOOL:-clang-format-14} # for super-linter v6 action
echo "clang-format check (git-tracked and non-ignored files only)"

mapfile -t CLANG_FORMAT_FILES < <(
	git ls-files --cached --others --exclude-standard \
		-- '*.cpp' -- '*.hpp' -- '*.cc' -- '*.c' -- '*.h' \
		':!:pymtl_wrap.c' ':!:vmlinux.h'
)

if ((${#CLANG_FORMAT_FILES[@]})); then
	# Run clang-format in parallel batches, avoiding git-ignored files.
	printf '%s\0' "${CLANG_FORMAT_FILES[@]}" |
		xargs -0 -n32 -P"$(nproc)" "${CLANG_FORMAT_TOOL}" -i
fi

isort python/ tests/
black python/ tests/
