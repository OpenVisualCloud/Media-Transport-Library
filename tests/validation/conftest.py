# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 Intel Corporation
# Media Communications Mesh

import datetime
import logging
import os
import shutil
import time
from typing import Dict

import subprocess

import pytest


def force_release_vfio_devices(vfio_groups=None, max_attempts=5):
    """
    Forcefully release VFIO devices by killing processes holding them.
    
    Args:
        vfio_groups: List of VFIO group numbers (e.g., [323, 324]). If None, releases all.
        max_attempts: Maximum number of kill attempts
        
    Returns:
        True if devices are free, False otherwise
    """
    for attempt in range(max_attempts):
        try:
            # Find PIDs of processes holding VFIO devices
            if vfio_groups:
                pattern = "|".join(str(g) for g in vfio_groups)
                cmd = f"sudo lsof -t 2>/dev/null /dev/vfio/{vfio_groups[0]} /dev/vfio/{vfio_groups[1]} 2>/dev/null | sort -u"
            else:
                cmd = "sudo lsof -t 2>/dev/null /dev/vfio/* 2>/dev/null | sort -u"
            
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=2)
            pids = result.stdout.strip().split('\n')
            pids = [p.strip() for p in pids if p.strip()]
            
            if not pids:
                # No processes holding VFIO - success!
                return True
            
            # Kill all processes holding VFIO devices
            logger.warning(f"Attempt {attempt + 1}: Killing {len(pids)} processes holding VFIO devices: {pids}")
            for pid in pids:
                try:
                    subprocess.run(f"sudo kill -9 {pid}", shell=True, timeout=2)
                except Exception:
                    pass
            
            # Wait a bit for kernel to release resources
            time.sleep(1)
            
        except Exception as e:
            logger.debug(f"Error checking VFIO devices: {e}")
    
    # Final check
    try:
        if vfio_groups:
            cmd = f"sudo lsof 2>/dev/null /dev/vfio/{vfio_groups[0]} /dev/vfio/{vfio_groups[1]} 2>/dev/null | wc -l"
        else:
            cmd = "sudo lsof 2>/dev/null /dev/vfio/* 2>/dev/null | wc -l"
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=2)
        count = int(result.stdout.strip() or 0)
        return count == 0
    except Exception:
        return False


def wait_for_vfio_release(vfio_groups=None, timeout=30, check_interval=0.2):
    """
    Wait until VFIO devices are no longer busy by checking if any process has them open.
    Will block until devices are free or timeout is reached.
    
    Args:
        vfio_groups: List of VFIO group numbers (e.g., [323, 324]). If None, checks all.
        timeout: Maximum time to wait in seconds (increased to 30s for reliability)
        check_interval: How often to check in seconds
        
    Returns:
        True if devices are free, False if timeout reached
    """
    start_time = time.time()
    
    while time.time() - start_time < timeout:
        try:
            # Check if any process has VFIO devices open
            if vfio_groups:
                # Check specific groups using lsof on the device files directly
                cmd = f"sudo lsof 2>/dev/null /dev/vfio/{vfio_groups[0]} /dev/vfio/{vfio_groups[1]} 2>/dev/null | wc -l"
            else:
                # Check all VFIO devices
                cmd = "sudo lsof 2>/dev/null /dev/vfio/* 2>/dev/null | wc -l"
            
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=2)
            count = int(result.stdout.strip() or 0)
            
            if count == 0:
                # No processes holding VFIO devices - we're good!
                logger.debug("VFIO devices are free")
                return True
            
            # Log every 2 seconds
            elapsed = time.time() - start_time
            if int(elapsed) % 2 == 0 and elapsed > 0:
                logger.debug(f"Waiting for VFIO release... ({int(elapsed)}s elapsed, {count} handles open)")
                
        except Exception as e:
            logger.debug(f"Error checking VFIO: {e}")
            
        time.sleep(check_interval)
    
    # Timeout reached
    logger.error(f"VFIO devices still busy after {timeout}s timeout!")
    return False


@pytest.fixture(autouse=True, scope="function")
def cleanup_rxtxapp_vfio(request):
    """
    Forcefully release VFIO devices before and after each test.
    Prevents DPDK/VFIO 'device or resource busy' errors.
    
    This fixture will BLOCK until devices are free or force-kill processes.
    """
    logger.debug("=== Pre-test VFIO cleanup ===")
    
    # Try to get PCI devices from the test if nic_port_list fixture is available
    pci_devices = ["0000:31:01.0", "0000:31:01.1"]  # Default fallback
    try:
        if "nic_port_list" in request.fixturenames:
            nic_list = request.getfixturevalue("nic_port_list")
            if nic_list:
                pci_devices = nic_list[:2]  # Use first 2 devices
                logger.debug(f"Using PCI devices from test: {pci_devices}")
    except Exception:
        pass  # Use defaults if we can't get the fixture
    
    # Step 1: Kill any RxTxApp processes
    try:
        result = subprocess.run("pkill -9 RxTxApp", shell=True, timeout=5, capture_output=True)
        if result.returncode == 0:
            logger.debug("Killed RxTxApp processes")
    except Exception:
        pass
    
    # Step 2: Force release VFIO devices by killing any processes holding them
    force_release_vfio_devices(vfio_groups=[323, 324], max_attempts=5)
    
    # Step 3: Wait until devices are absolutely free (BLOCKS until free or timeout)
    released = wait_for_vfio_release(vfio_groups=[323, 324], timeout=30)
    
    if not released:
        # Last resort: unbind and rebind the devices
        logger.error("VFIO devices still busy! Attempting unbind/rebind...")
        try:
            subprocess.run("sudo dpdk-devbind.py --unbind 0000:31:01.0 0000:31:01.1", shell=True, timeout=5)
            time.sleep(2)
            subprocess.run("sudo dpdk-devbind.py --bind=vfio-pci 0000:31:01.0 0000:31:01.1", shell=True, timeout=5)
            time.sleep(1)
        except Exception as e:
            logger.error(f"Unbind/rebind failed: {e}")
    else:
        logger.debug("VFIO devices successfully released")
    
    yield  # Run the test
    
    logger.debug("=== Post-test VFIO cleanup ===")
    
    # After test completes, force release again
    try:
        subprocess.run("pkill -9 RxTxApp", shell=True, timeout=5)
    except Exception:
        pass
    
    force_release_vfio_devices(vfio_groups=[323, 324], max_attempts=5)
    wait_for_vfio_release(vfio_groups=[323, 324], timeout=30)
    
    # Reset PCI devices to ensure they're not busy for next test
    for device in pci_devices:
        try:
            reset_cmd = f"sudo bash -c 'echo 1 > /sys/bus/pci/devices/{device}/reset'"
            result = subprocess.run(reset_cmd, shell=True, timeout=5, capture_output=True)
            if result.returncode == 0:
                logger.debug(f"Successfully reset PCI device {device}")
            else:
                logger.warning(f"Failed to reset PCI device {device}: {result.stderr.decode()}")
        except Exception as e:
            logger.warning(f"Exception while resetting PCI device {device}: {e}")
        time.sleep(0.5)  # Small delay between resets
from common.mtl_manager.mtlManager import MtlManager
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
from mtl_engine.vfio_manager import get_vfio_manager
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
def build(test_config: dict, mtl_path: str) -> str:
    """Get build directory path.
    
    Returns the build directory from test_config if available,
    otherwise defaults to mtl_path/build.
    """
    return test_config.get("build", f"{mtl_path}/build")


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
def nic_port_list(hosts: dict, mtl_path) -> list:
    """Get list of VF PCI addresses for testing.
    
    Returns:
        List of VF PCI addresses (e.g., ['0000:31:01.0', '0000:31:01.1', ...])
    """
    vfs_list = []
    for host in hosts.values():
        nicctl = Nicctl(mtl_path, host)
        if int(host.network_interfaces[0].virtualization.get_current_vfs()) == 0:
            vfs = nicctl.create_vfs(host.network_interfaces[0].pci_address.lspci)
        vfs = nicctl.vfio_list(host.network_interfaces[0].pci_address.lspci)
        # Store VFs on the host object for later use
        host.vfs = vfs
        vfs_list.extend(vfs)
    return vfs_list


@pytest.fixture(scope="session")
def test_time(test_config: dict) -> int:
    test_time = test_config.get("test_time", 30)
    return test_time


@pytest.fixture(autouse=True)
def delay_between_tests(request):
    """Wait between tests to allow resources to be released."""
    yield  # Test runs here
    
    # After test completes, add delay
    try:
        test_config = request.getfixturevalue("test_config")
        time_sleep = test_config.get("delay_between_tests", 5)
    except Exception:
        time_sleep = 10
    
    time.sleep(time_sleep)


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
        cmd = f"sudo cp {src_media_file_path} {ramdisk_media_file_path}"
        try:
            host.connection.execute_command(cmd)
        except ConnectionCalledProcessError as e:
            logging.log(
                level=logging.ERROR, msg=f"Failed to execute command {cmd}: {e}"
            )
    yield media_file_info, ramdisk_media_file_path
    for host in hosts.values():
        cmd = f"sudo rm {ramdisk_media_file_path}"
        try:
            host.connection.execute_command(cmd)
        except ConnectionCalledProcessError as e:
            logging.log(
                level=logging.ERROR, msg=f"Failed to execute command {cmd}: {e}"
            )


@pytest.fixture(scope="session", autouse=True)
def mtl_manager(hosts):
    """
    Automatically start MtlManager on all hosts at the beginning of the test session,
    and stop it at the end.
    """
    managers = {}
    for host in hosts.values():
        mgr = MtlManager(host)
        if not mgr.start():
            raise RuntimeError(f"Failed to start MtlManager on host {host.name}")
        managers[host.name] = mgr
    yield managers
    for mgr in managers.values():
        mgr.stop()


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
