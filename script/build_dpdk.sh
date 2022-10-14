#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# Run from top, for CI job

# check out dpdk code
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v22.07
git switch -c v22.07

# apply the patches
git am ../patches/dpdk/22.07/0001-pcapng-add-ns-timestamp-for-copy-api.patch
git am ../patches/dpdk/22.07/0002-net-af_xdp-parse-numa-node-id-from-sysfs.patch
git am ../patches/dpdk/22.07/0003-net-iavf-refine-queue-rate-limit-configure.patch
git am ../patches/dpdk/22.07/0004-net-ice-revert-PF-ICE-rate-limit-to-non-queue-group-.patch
git am ../patches/dpdk/22.07/0005-net-iavf-support-max-burst-size-configuration.patch
git am ../patches/dpdk/22.07/0006-net-ice-support-max-burst-size-configuration.patch
git am ../patches/dpdk/22.07/0007-Add-support-for-i225-IT-ethernet-device-into-igc-pmd.patch
git am ../patches/dpdk/22.07/0008-Change-to-enable-PTP.patch
git am ../patches/dpdk/22.07/0009-ice-fix-ice_interrupt_handler-panic-when-stop.patch

# build and install dpdk now
meson build
ninja -C build
cd build
sudo ninja install