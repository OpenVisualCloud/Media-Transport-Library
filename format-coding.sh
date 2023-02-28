#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Based on clang-format, shellcheck
# For ubuntu, pls "apt-get install clang-format shellcheck"

set -e

echo "clang-format check"
find . -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' -exec clang-format --verbose -i {} \;

#echo "shell check"
#find ./ -name "*.sh" -exec shellcheck {} \;

# hadolint check
# hadolint docker/ubuntu.dockerfile

# actionlint check
# actionlint

# markdownlint check
# docker run -v $PWD:/workdir --rm ghcr.io/igorshubovych/markdownlint-cli:latest "*.md" -f
# find ./ -name "*.md" -exec markdownlint {} --fix \;

# textlint
# find ./ -name "*.md" -exec textlint {} \;
# find ./ -name "*.md" -exec textlint {} --fix \;
