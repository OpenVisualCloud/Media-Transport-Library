#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# check out msdk code
git clone https://github.com/Intel-Media-SDK/MediaSDK.git
cd MediaSDK || exit
git checkout intel-mediasdk-22.6.4
git switch -c v22.6.4

# apply the patches
git am ../*.patch

# build now
cmake -S . -B build -G Ninja -DENABLE_IMTL=ON
cmake --build build
