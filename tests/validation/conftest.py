# # SPDX-License-Identifier: BSD-3-Clause
# # Copyright 2024-2025 Intel Corporation
# # Media Communications Mesh
import logging
import os
import time
from pathlib import Path
from typing import Dict

import pytest
from common.nicctl import Nicctl
from create_pcap_file.ramdisk import RamdiskPreparer
from mtl_engine.stash import clear_result_media, remove_result_media

logger = logging.getLogger(__name__)
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
def media(test_config: dict) -> str:
    media = test_config.get("media_path", "/opt/intel/media")
    os.environ["media"] = media
    return media


@pytest.fixture(scope="session")
def build(mtl_path):
    return mtl_path


@pytest.fixture(scope="session")
def mtl_path(test_config: dict) -> str:
    return test_config.get("mtl_path", "/opt/intel/mcm/_build/mtl/")


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
def nic_port_list(hosts: dict, mtl_path) -> list:
    for host in hosts.values():
        connection = host.connection
        # connection.enable_sudo()
        nicctl = Nicctl(mtl_path, host)
        if int(host.network_interfaces[0].virtualization.get_current_vfs()) == 0:
            vfs = nicctl.create_vfs(host.network_interfaces[0].pci_address)
        vfs = nicctl.vfio_list()
        # Store VFs on the host object for later use
        host.vfs = vfs
        # connection.disable_sudo()


# def nic_port_list(request, media_config, hosts: dict = None) -> list:
# vfs = []
# if hosts:
#     for host in hosts.values():
#         if hasattr(host, "vfs"):
#             vfs.extend(host.vfs)
# if vfs:
#     return vfs
# # Fallback: use --nic parameter
# nic_option = request.config.getoption("--nic")
# if nic_option:
#     return [nic.strip() for nic in nic_option.split(",") if nic.strip()]
# raise RuntimeError("No VFs found and --nic parameter not provided!")


@pytest.fixture(scope="session")
def test_time(test_config: dict) -> int:
    test_time = test_config.get("test_time", 30)
    return test_time


@pytest.fixture(autouse=True)
def delay_between_tests(test_config: dict):
    time_sleep = test_config.get("delay_between_tests", 3)
    time.sleep(time_sleep)
    yield


@pytest.fixture
def prepare_ramdisk(hosts, test_config):
    ramdisk_cfg = test_config.get("ramdisk", {})
    capture_cfg = test_config.get("capture_cfg", {})
    pcap_dir = ramdisk_cfg.get("pcap_dir", "/home/pcap_files")
    tmpfs_size = ramdisk_cfg.get("tmpfs_size", "768G")
    tmpfs_name = ramdisk_cfg.get("tmpfs_name", "new_disk_name")
    use_sudo = ramdisk_cfg.get("use_sudo", True)

    if capture_cfg.get("enable", False):
        for host in hosts.values():
            preparer = RamdiskPreparer(
                host=host,
                pcap_dir=pcap_dir,
                tmpfs_size=tmpfs_size,
                tmpfs_name=tmpfs_name,
                use_sudo=use_sudo,
            )
            preparer.start()


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
