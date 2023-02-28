#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Based on clang-format
# For ubuntu, pls "apt-get install clang-format"

set -e

echo "clang-format check"
find . -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' -exec clang-format --verbose -i {} \;
