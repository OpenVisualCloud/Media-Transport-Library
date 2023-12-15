#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Based on clang-format
# For ubuntu, pls "apt-get install clang-format"

set -e

echo "clang-format check"
find . -path ./build -prune -o -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' ! -name 'pymtl_wrap.c' -exec clang-format --verbose -i {} +

black python/
isort python/
