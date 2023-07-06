#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

# check out librist code
rm librist -rf
git clone https://code.videolan.org/rist/librist.git
cd librist
git checkout 9f09a3defd6e59839aae3e3b7b5411213fa04b8a

# apply the patches
git am ../*.patch

# build now
meson build
ninja -C build
