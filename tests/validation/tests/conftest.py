# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
pytest_plugins = ["tests.Engine.fixtures", "tests.Engine.logging"]


def pytest_addoption(parser):
    parser.addoption(
        "--keep", help="keep result media files: all, failed, none (default)"
    )
    parser.addoption(
        "--dmesg", help="method of dmesg gathering: clear (dmesg -C), keep (default)"
    )
    parser.addoption("--media", help="path to media asset (default /mnt/media)")
    parser.addoption(
        "--build", help="path to build (default ../Media-Transport-Library)"
    )
    parser.addoption("--nic", help="list of PCI IDs of network devices")
    parser.addoption("--dma", help="list of PCI IDs of DMA devices")
    parser.addoption("--time", help="seconds to run every test (default=15)")
