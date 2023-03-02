#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

# check out librist code
rm librist -rf
git clone https://code.videolan.org/rist/librist.git
cd librist

# apply the patches
git am ../*.patch

# build now
./build_with_mtl.sh
