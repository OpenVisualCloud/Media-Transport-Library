#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Based on clang-format
# For ubuntu, pls "apt-get install clang-format"

set -e

# copy .clang-format to the cwd for linting process
cp .github/linters/.clang-format .clang-format

echo "clang-format check"
find . -path ./build -prune -o -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' ! -name 'pymtl_wrap.c' \
	! -name 'vmlinux.h' ! -name '.clang-format' \
	-exec clang-format-17 --verbose -i {} +

# clean-up the copied .clang-format
rm -rf	.clang-format
CONFIG=".github/linters/.pyproject.toml"
black --config "$CONFIG" python/
isort --settings-path "$CONFIG" python/
