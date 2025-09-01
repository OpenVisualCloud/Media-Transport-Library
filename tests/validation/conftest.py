# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 Intel Corporation
# Media Communications Mesh

import datetime
import logging
import os
import shutil
import time
from typing import Dict

import pytest
from common.nicctl import Nicctl
from mfd_common_libs.custom_logger import add_logging_level
from mfd_common_libs.log_levels import TEST_FAIL, TEST_INFO, TEST_PASS
from mfd_connect.exceptions import ConnectionCalledProcessError
from mtl_engine.const import LOG_FOLDER, TESTCMD_LVL
from mtl_engine.csv_report import (
    csv_add_test,
    csv_write_report,
    update_compliance_result,
)
from mtl_engine.ramdisk import Ramdisk
from mtl_engine.stash import (
    clear_issue,
    clear_result_log,
    clear_result_media,
    clear_result_note,
    get_issue,
    get_result_note,
    remove_result_media,
)
from pytest_mfd_logging.amber_log_formatter import AmberLogFormatter

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
    keep = request.config.getoption("--keep", None)
    if keep is None:
        keep = "none"
    if keep.lower() not in ["all", "failed", "none"]:
        raise RuntimeError(f"Wrong option --keep={keep}")
    os.environ["keep"] = keep.lower()
    return keep.lower()


@pytest.fixture(scope="session", autouse=True)
def dmesg(request):
    dmesg = request.config.getoption("--dmesg", None)
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
def nic_port_list(hosts: dict, mtl_path) -> None:
    for host in hosts.values():
        nicctl = Nicctl(mtl_path, host)
        if int(host.network_interfaces[0].virtualization.get_current_vfs()) == 0:
            vfs = nicctl.create_vfs(host.network_interfaces[0].pci_address.lspci)
        vfs = nicctl.vfio_list(host.network_interfaces[0].pci_address.lspci)
        # Store VFs on the host object for later use
        host.vfs = vfs


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
    tmpfs_size_gib = ramdisk_cfg.get("tmpfs_size_gib", "768")

    if capture_cfg.get("enable", False):
        ramdisks = [
            Ramdisk(host=host, mount_point=pcap_dir, size_gib=tmpfs_size_gib)
            for host in hosts.values()
        ]
        for ramdisk in ramdisks:
            ramdisk.mount()


@pytest.fixture(scope="session")
def media_ramdisk(hosts, test_config):
    ramdisk_config = test_config.get("ramdisk", {}).get("media", {})
    ramdisk_mountpoint = ramdisk_config.get("mountpoint", "/mnt/ramdisk/media")
    ramdisk_size_gib = ramdisk_config.get("size_gib", 32)
    ramdisks = [
        Ramdisk(host=host, mount_point=ramdisk_mountpoint, size_gib=ramdisk_size_gib)
        for host in hosts.values()
    ]
    for ramdisk in ramdisks:
        ramdisk.mount()
    yield
    for ramdisk in ramdisks:
        ramdisk.unmount()


@pytest.fixture(scope="function")
def media_file(media_ramdisk, request, hosts, test_config):
    media_file_info = request.param
    ramdisk_config = test_config.get("ramdisk", {}).get("media", {})
    ramdisk_mountpoint = ramdisk_config.get("mountpoint", "/mnt/ramdisk/media")
    media_path = test_config.get("media_path", "/mnt/media")
    src_media_file_path = os.path.join(media_path, media_file_info["filename"])
    ramdisk_media_file_path = os.path.join(
        ramdisk_mountpoint, media_file_info["filename"]
    )
    for host in hosts.values():
        cmd = f"cp {src_media_file_path} {ramdisk_media_file_path}"
        try:
            host.connection.execute_command(cmd)
        except ConnectionCalledProcessError as e:
            logging.log(
                level=logging.ERROR, msg=f"Failed to execute command {cmd}: {e}"
            )
    yield media_file_info, ramdisk_media_file_path
    for host in hosts.values():
        cmd = f"rm {ramdisk_media_file_path}"
        try:
            host.connection.execute_command(cmd)
        except ConnectionCalledProcessError as e:
            logging.log(
                level=logging.ERROR, msg=f"Failed to execute command {cmd}: {e}"
            )


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


@pytest.fixture(scope="session", autouse=True)
def log_session():
    add_logging_level("TESTCMD", TESTCMD_LVL)

    today = datetime.datetime.today()
    folder = today.strftime("%Y-%m-%dT%H:%M:%S")
    path = os.path.join(LOG_FOLDER, folder)
    path_symlink = os.path.join(LOG_FOLDER, "latest")
    try:
        os.remove(path_symlink)
    except FileNotFoundError:
        pass
    os.makedirs(path, exist_ok=True)
    os.symlink(folder, path_symlink)
    yield
    if os.path.exists("pytest.log"):
        shutil.copy("pytest.log", f"{LOG_FOLDER}/latest/pytest.log")
    else:
        logging.warning("pytest.log not found, skipping copy")
    csv_write_report(f"{LOG_FOLDER}/latest/report.csv")


@pytest.fixture(scope="session", autouse=True)
def compliance_report(request, log_session, test_config):
    """
    This function is used for compliance check and report.
    """
    # TODO: Implement compliance check logic. When tcpdump pcap is enabled, at the end of the test session all pcaps
    # shall be send into EBU list.
    # Pcaps shall be stored in the ramdisk, and then moved to the compliance
    # folder or send into EBU list after each test finished and remove it from the ramdisk.
    # Compliance report generation logic goes here after yield. Or in another class / function but triggered here.
    # AFAIK names of pcaps contains test name so it can be matched with result of each test like in code below.
    yield
    if test_config.get("compliance", False):
        logging.info("Compliance mode enabled, updating compliance results")
        for item in request.session.items:
            test_case = item.nodeid
            update_compliance_result(test_case, "Fail")


@pytest.fixture(scope="function", autouse=True)
def log_case(request, caplog: pytest.LogCaptureFixture):
    case_id = request.node.nodeid
    case_folder = os.path.dirname(case_id)
    os.makedirs(os.path.join(LOG_FOLDER, "latest", case_folder), exist_ok=True)
    logfile = os.path.join(LOG_FOLDER, "latest", f"{case_id}.log")
    fh = logging.FileHandler(logfile)
    formatter = request.session.config.pluginmanager.get_plugin(
        "logging-plugin"
    ).formatter
    format = AmberLogFormatter(formatter)
    fh.setFormatter(format)
    fh.setLevel(logging.DEBUG)
    logger = logging.getLogger()
    logger.addHandler(fh)
    clear_result_log()
    clear_issue()
    yield
    report = request.node.stash[phase_report_key]
    if report["setup"].failed:
        logging.log(level=TEST_FAIL, msg=f"Setup failed for {case_id}")
        os.chmod(logfile, 0o4755)
        result = "Fail"
    elif ("call" not in report) or report["call"].failed:
        logging.log(level=TEST_FAIL, msg=f"Test failed for {case_id}")
        os.chmod(logfile, 0o4755)
        result = "Fail"
    elif report["call"].passed:
        logging.log(level=TEST_PASS, msg=f"Test passed for {case_id}")
        os.chmod(logfile, 0o755)
        result = "Pass"
    else:
        logging.log(level=TEST_INFO, msg=f"Test skipped for {case_id}")
        result = "Skip"

    logger.removeHandler(fh)

    commands = []
    for record in caplog.get_records("call"):
        if record.levelno == TESTCMD_LVL:
            commands.append(record.message)

    csv_add_test(
        test_case=case_id,
        commands="\n".join(commands),
        result=result,
        issue=get_issue(),
        result_note=get_result_note(),
    )

    clear_result_note()
