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
import pytest
import os
from typing import Dict

from .stash import clear_result_media, remove_result_media

phase_report_key = pytest.StashKey[Dict[str, pytest.CollectReport]]()


@pytest.hookimpl(wrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    # execute all other hooks to obtain the report object
    rep = yield

    # store test results for each phase of a call, which can
    # be "setup", "call", "teardown"
    item.stash.setdefault(phase_report_key, {})[rep.when] = rep

    return rep


@pytest.fixture(scope="session")
def media(request):
    media = request.config.getoption("--media")
    if media is None:
        media = "/mnt/media"
    os.environ["media"] = media
    return media


@pytest.fixture(scope="session")
def build(request):
    build = request.config.getoption("--build")
    if build is None:
        build = "../Media-Transport-Library"
    os.environ["build"] = build
    return build


@pytest.fixture(scope="session", autouse=True)
def keep(request):
    keep = request.config.getoption("--keep")
    if keep is None:
        keep = "none"
    if keep.lower() not in ["all", "failed", "none"]:
        raise RuntimeError(f"Wrong option --keep={keep}")
    os.environ["keep"] = keep.lower()
    return keep.lower()


@pytest.fixture(scope="session", autouse=True)
def dmesg(request):
    dmesg = request.config.getoption("--dmesg")
    if dmesg is None:
        dmesg = "keep"
    if dmesg.lower() not in ["clear", "keep"]:
        raise RuntimeError(f"Wrong option --dmesg={dmesg}")
    os.environ["dmesg"] = dmesg.lower()
    return dmesg.lower()


@pytest.fixture(scope="function", autouse=True)
def fixture_remove_result_media(request):
    clear_result_media()
    yield

    if os.environ["keep"] == "all":
        return
    if os.environ["keep"] == "failed":
        report = request.node.stash[phase_report_key]
        if "call" in report and report["call"].failed:
            return
    remove_result_media()


@pytest.fixture(scope="session")
def dma_port_list(request):
    dma = request.config.getoption("--dma")
    assert dma is not None, "--dma parameter not provided"
    return dma.split(",")


@pytest.fixture(scope="session")
def nic_port_list(request):
    nic = request.config.getoption("--nic")
    assert nic is not None, "--nic parameter not provided"
    return nic.split(",")


@pytest.fixture(scope="session")
def test_time(request):
    test_time = request.config.getoption("--time")
    if test_time is None:
        return 30
    return int(test_time)
