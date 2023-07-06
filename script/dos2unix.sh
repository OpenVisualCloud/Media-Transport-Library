#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

echo "dos2unix check"
find . -regex '.*' -exec dos2unix {} \;
