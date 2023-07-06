#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

set -e

echo "Change P interface to 0000:af:00.0"
sed -i 's/0000:af:01.0/0000:af:00.0/g' ./*.json
echo "Change R interface to 0000:af:00.1"
sed -i 's/0000:af:01.1/0000:af:00.1/g' ./*.json
