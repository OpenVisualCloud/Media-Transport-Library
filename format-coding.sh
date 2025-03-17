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

set -e

CLANG_FORMAT_TOOL="clang-format-14" # for super-linter v6 action
echo "clang-format check"
find . -path ./build -prune -o -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' ! -name 'pymtl_wrap.c' \
	! -name 'vmlinux.h' -exec ${CLANG_FORMAT_TOOL} --verbose -i {} +

black python/
isort python/
