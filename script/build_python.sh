#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

cd python/swig
swig -python -I/usr/local/include -o pymtl_wrap.c pymtl.i
python3 setup.py build_ext --inplace
sudo python3 setup.py install
cd ../../
