#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022-2025 Intel Corporation

SCRIPT_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"
set -e -o pipefail

mkdir -p "${SCRIPT_DIR}/MediaSDK"
curl -Lf "https://github.com/Intel-Media-SDK/MediaSDK/archive/refs/tags/intel-mediasdk-23.2.2.tar.gz" -o "${SCRIPT_DIR}/intel-mediasdk-22.6.4.tar.gz"
tar -zx --strip-components=1 -C "${SCRIPT_DIR}/MediaSDK" -f "${SCRIPT_DIR}/intel-mediasdk-22.6.4.tar.gz"
rm -f "${SCRIPT_DIR}/intel-mediasdk-22.6.4.tar.gz"
patch -d "${SCRIPT_DIR}/MediaSDK" -p1 -i <(cat "${SCRIPT_DIR}/"*.patch)

cmake \
	-S "${SCRIPT_DIR}/MediaSDK" \
	-B "${SCRIPT_DIR}/build" \
	-G Ninja \
	-DENABLE_IMTL=ON \
	-DCMAKE_INSTALL_PREFIX="${SCRIPT_DIR}/_install"
ninja -C "${SCRIPT_DIR}/build"
ninja -C "${SCRIPT_DIR}/build" install

echo "Use sample applications, found under path:"
echo "LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH}:${SCRIPT_DIR}/_install/lib\""
echo "APPLICATION_PATH=\"${SCRIPT_DIR}/_install/share/mfx/samples/sample_encode\""
