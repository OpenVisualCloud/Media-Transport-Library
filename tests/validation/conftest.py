# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 Intel Corporation
# Media Communications Mesh

import datetime
import logging
import os
import shutil
import signal
import time
from typing import Any, Dict

import pytest
from common.mtl_manager.mtlManager import MtlManager
from common.nicctl import InterfaceSetup, Nicctl
from compliance.compliance_client import PcapComplianceClient
from create_pcap_file.netsniff import NetsniffRecorder, calculate_packets_per_frame
from mfd_common_libs.custom_logger import add_logging_level
from mfd_common_libs.log_levels import TEST_FAIL, TEST_INFO, TEST_PASS
from mfd_connect.exceptions import ConnectionCalledProcessError
from mtl_engine import ip_pools
from mtl_engine.const import FRAMES_CAPTURE, LOG_FOLDER, TESTCMD_LVL
from mtl_engine.csv_report import (
    csv_add_test,
    csv_write_report,
    get_compliance_result,
    update_compliance_result,
)
from mtl_engine.execute import log_fail
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


def _select_sniff_interface(host, capture_cfg: dict):
    def _pci_device_id(nic) -> str:
        """Return lowercased PCI vendor:device identifier (e.g., '8086:1592')."""
        return f"{nic.pci_device.vendor_id}:{nic.pci_device.device_id}".lower()

    sniff_interface = capture_cfg.get("sniff_interface")
    if sniff_interface:
        for nic in host.network_interfaces:
            if nic.name == str(sniff_interface):
                return nic
        available = [
            f"{nic.name} ({nic.pci_address.lspci})" for nic in host.network_interfaces
        ]
        raise RuntimeError(
            f"capture_cfg.sniff_interface={sniff_interface} not found on host {host.name}. "
            f"Available interfaces: {', '.join(available)}"
        )

    sniff_interface_index = capture_cfg.get("sniff_interface_index")
    if sniff_interface_index is not None:
        return host.network_interfaces[int(sniff_interface_index)]

    sniff_pci_device = capture_cfg.get("sniff_pci_device")
    if sniff_pci_device:
        target = str(sniff_pci_device).lower()

        direct_matches = [
            nic for nic in host.network_interfaces if target == _pci_device_id(nic)
        ]
        if direct_matches:
            return direct_matches[1] if len(direct_matches) > 1 else direct_matches[0]

        available = [
            f"{nic.name} ({nic.pci_address.lspci})" for nic in host.network_interfaces
        ]
        raise RuntimeError(
            f"capture_cfg.sniff_pci_device={sniff_pci_device} not found on host {host.name}. "
            f"Available interfaces: {', '.join(available)}"
        )

    # Default behavior: capture on 2nd PF.
    if len(host.network_interfaces) < 2:
        raise RuntimeError(
            f"Host {host.name} has less than 2 network interfaces; "
            f"Cannot select 2nd PF for capture. Add more interfaces to config or turn off capture."
        )

    return host.network_interfaces[1]


def _select_sniff_interface_name(host, capture_cfg: dict) -> str:
    return _select_sniff_interface(host, capture_cfg).name


def _select_capture_host(hosts: dict):
    return hosts["client"] if "client" in hosts else list(hosts.values())[0]


@pytest.fixture(scope="function")
def ptp_sync(request, test_config: dict, hosts):
    """Start phc2sys or ptp4l for the capture interface before tests.

    - Uses the same interface selection logic as PCAP capture.
    - For tests marked with @pytest.mark.ptp: starts ptp4l for PTP synchronization.
    - For other tests: detects PTP Hardware Clock via `ethtool -T` and runs phc2sys.

    The process is stopped at the end of the test.
    """
    capture_cfg = test_config.get("capture_cfg", {})
    if not (capture_cfg and capture_cfg.get("enable")):
        yield
        return

    host = _select_capture_host(hosts)
    sniff_nic = _select_sniff_interface(host, capture_cfg)
    capture_iface = sniff_nic.name

    # For tests marked with @pytest.mark.ptp, start ptp4l instead of phc2sys
    if request.node.get_closest_marker("ptp"):
        logger.info(f"Starting ptp4l for PTP synchronization (iface={capture_iface})")

        log_path = f"/tmp/ptp4l-{capture_iface}.log"
        ptp4l_cmd = f"sudo ptp4l -i '{capture_iface}' -s -m -2"
        ptp4l_process = host.connection.start_process(
            ptp4l_cmd,
            stderr_to_stdout=True,
            output_file=log_path,
        )

        # Give ptp4l a moment to fail fast (e.g., missing interface).
        time.sleep(0.2)
        if not ptp4l_process.running:
            raise RuntimeError(
                f"Failed to start ptp4l (iface={capture_iface}). log={log_path}"
            )

        try:
            yield
        finally:
            if not ptp4l_process:
                return

            if not ptp4l_process.running:
                raise RuntimeError(
                    f"ptp4l process (iface={capture_iface}) "
                    f"stopped unexpectedly. See log: {log_path}"
                )

            ptp4l_process.kill(wait=None, with_signal=signal.SIGTERM)
        return

    # For non-PTP tests, start phc2sys
    ptp_details = host.connection.execute_command(
        f"sudo ethtool -T '{capture_iface}' 2>/dev/null || true"
    )
    ptp_idx = ""
    for line in (ptp_details.stdout or "").splitlines():
        # Keep this equivalent to: awk -F': ' '/PTP Hardware Clock:/ {print $2; exit}'
        if "PTP Hardware Clock:" in line:
            ptp_idx = line.split(": ", 1)[1].strip() if ": " in line else ""
            break

    if not ptp_idx.isdigit():
        raise RuntimeError(
            "ERROR: failed to parse PTP Hardware Clock index for "
            f"{capture_iface}. Details: {ptp_details.stdout}{ptp_details.stderr}"
        )

    capture_ptp = f"/dev/ptp{ptp_idx}"

    logger.info(
        f"Starting phc2sys: {capture_ptp} -> CLOCK_REALTIME (iface={capture_iface})"
    )

    log_path = f"/tmp/phc2sys-{capture_iface}.log"
    phc2sys_cmd = "sudo phc2sys " f"-s '{capture_ptp}' -c CLOCK_REALTIME -O 0 -m"
    phc2sys_process = host.connection.start_process(
        phc2sys_cmd,
        stderr_to_stdout=True,
        output_file=log_path,
    )

    # Give phc2sys a moment to fail fast (e.g., missing /dev/ptpX permissions).
    time.sleep(0.2)
    if not phc2sys_process.running:
        raise RuntimeError(
            f"Failed to start phc2sys (iface={capture_iface}, ptp={capture_ptp}). "
            f"log={log_path}"
        )

    try:
        yield
    finally:
        if not phc2sys_process:
            return

        if not phc2sys_process.running:
            raise RuntimeError(
                f"phc2sys process (iface={capture_iface}, ptp={capture_ptp}) "
                f"stopped unexpectedly. See log: {log_path}"
            )

        phc2sys_process.kill(wait=None, with_signal=signal.SIGTERM)


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
def mtl_path(test_config: dict) -> str:
    mtl_path = test_config.get("mtl_path")
    if not mtl_path:
        raise RuntimeError("mtl_path not specified in test config")
    return mtl_path


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


@pytest.fixture(scope="function")
def setup_interfaces(hosts, test_config, mtl_path):
    interface_setup = InterfaceSetup(hosts, mtl_path)
    yield interface_setup
    interface_setup.cleanup()


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

    ramdisks = []
    if capture_cfg.get("enable", False):
        ramdisks = [
            Ramdisk(host=host, mount_point=pcap_dir, size_gib=tmpfs_size_gib)
            for host in hosts.values()
        ]
        for ramdisk in ramdisks:
            ramdisk.mount()
    yield
    for ramdisk in ramdisks:
        ramdisk.unmount()


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


"""Fixture that copies requested media files into the media ramdisk for each test host.

When `request.param` is provided, the referenced media file is copied from the
configured media library into the RAM disk (creating the RAM disk entry, if
needed) for every host before the test runs, and automatically removed
afterward. If no `request.param` is supplied, the fixture simply yields the
RAM disk mount path without staging any file.

Args:
    media_ramdisk: Pytest fixture ensuring the media RAM disk is available.
    request: Pytest request object; `request.param` describes the media file to copy.
    hosts: Mapping of host identifiers to connection objects used to run shell commands.
    test_config: Dictionary containing test configuration (media paths, RAM disk mount point).

Yields:
    Tuple[dict | None, str]: `(media_file_info, ramdisk_path)` where `media_file_info`
    is the metadata for the staged file, or `None` if no media file was requested,
    and `ramdisk_path` is either the staged file path or the mount path alone when
    no file is staged.
"""


class OutputFileTracker:
    """Helper class to track and clean up output files created during tests."""

    def __init__(self, hosts):
        self._hosts = hosts
        self._files: list[str] = []

    def register(self, file_path: str) -> str:
        """Register an output file for automatic cleanup. Returns the path for convenience."""
        self._files.append(file_path)
        return file_path

    def cleanup(self):
        """Remove all registered output files from all hosts."""
        for file_path in self._files:
            for host in self._hosts.values():
                cmd = f"sudo rm -f {file_path}"
                try:
                    host.connection.execute_command(cmd)
                except ConnectionCalledProcessError as e:
                    logging.log(
                        level=logging.WARNING,
                        msg=f"Failed to remove output file {file_path}: {e}",
                    )
        self._files.clear()


@pytest.fixture(scope="function")
def output_files(hosts):
    """Fixture that provides automatic cleanup of output files created during tests.

    Usage:
        def test_example(output_files, ...):
            out_path = output_files.register("/path/to/output.file")
            # use out_path in your test
            # file will be automatically removed after test completes
    """
    tracker = OutputFileTracker(hosts)
    yield tracker
    tracker.cleanup()


@pytest.fixture(scope="function")
def media_file(media_ramdisk, request, hosts, test_config, output_files):
    media_file_info = getattr(request, "param", None)

    ramdisk_config = test_config.get("ramdisk", {}).get("media", {})
    ramdisk_mountpoint = ramdisk_config.get("mountpoint", "/mnt/ramdisk/media")
    media_path = test_config.get("media_path", "/mnt/media")

    # simple path where no media file is needed (e.g., generated files)
    if media_file_info is None:
        yield media_file_info, ramdisk_mountpoint
        return

    src_media_file_path = os.path.join(media_path, media_file_info["filename"])
    ramdisk_media_file_path = output_files.register(
        os.path.join(ramdisk_mountpoint, media_file_info["filename"])
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


@pytest.fixture(scope="function")
def pcap_capture(
    request, media_file, test_config, hosts, mtl_path, ptp_sync, prepare_ramdisk
):
    """Fixture for capturing pcap files during tests.

    Note: This fixture depends on prepare_ramdisk to ensure proper cleanup order.
    The netsniff-ng process must be stopped BEFORE the ramdisk is unmounted,
    otherwise the unmount will fail with 'device busy'.
    """
    capture_cfg = test_config.get("capture_cfg", {})
    capturer = None
    if capture_cfg and capture_cfg.get("enable"):
        host = hosts["client"] if "client" in hosts else list(hosts.values())[0]
        media_file_info, _ = media_file
        test_name = request.node.name
        if "frames_number" not in capture_cfg and "capture_time" not in capture_cfg:
            capture_cfg["packets_number"] = (
                FRAMES_CAPTURE * calculate_packets_per_frame(media_file_info)
            )
            logger.info(
                f"Capture {capture_cfg['packets_number']} packets for {FRAMES_CAPTURE} frames"
            )
        elif "frames_number" in capture_cfg:
            capture_cfg["packets_number"] = capture_cfg[
                "frames_number"
            ] * calculate_packets_per_frame(media_file_info)
            logger.info(
                f"Capture {capture_cfg['packets_number']} packets for {capture_cfg['frames_number']} frames"
            )
        capturer = NetsniffRecorder(
            host=host,
            test_name=test_name,
            pcap_dir=capture_cfg.get("pcap_dir", "/tmp"),
            interface=_select_sniff_interface_name(host, capture_cfg),
            silent=capture_cfg.get("silent", True),
            packets_capture=capture_cfg.get("packets_number", None),
            capture_time=capture_cfg.get("capture_time", None),
        )
    try:
        yield capturer
    finally:
        # Process compliance check if we have a captured pcap file
        if capturer and capturer.pcap_file:
            ebu_server = test_config.get("ebu_server", {})
            if not ebu_server:
                logger.error("EBU server configuration not found in test_config.yaml")
            else:
                ebu_ip = ebu_server.get("ebu_ip", None)
                ebu_login = ebu_server.get("user", None)
                ebu_passwd = ebu_server.get("password", None)
                ebu_proxy = ebu_server.get("proxy", None)
                proxy_cmd = f" --proxy {ebu_proxy}" if ebu_proxy else ""
                compliance_upl = capturer.host.connection.execute_command(
                    "python3 ./tests/validation/compliance/upload_pcap.py"
                    f" --ip {ebu_ip}"
                    f" --user {ebu_login}"
                    f" --password {ebu_passwd}"
                    f" --pcap '{capturer.pcap_file}'{proxy_cmd}",
                    cwd=f"{str(mtl_path)}",
                )
                if compliance_upl.return_code != 0:
                    logger.error(f"PCAP upload failed: {compliance_upl.stderr}")
                else:
                    uuid = compliance_upl.stdout.split(">>>UUID: ")[1].strip()
                    logger.debug(
                        f"PCAP successfully uploaded to EBU LIST with UUID: {uuid}"
                    )
                    uploader = PcapComplianceClient(
                        ebu_ip=ebu_ip,
                        user=ebu_login,
                        password=ebu_passwd,
                        pcap_id=uuid,
                        proxies={"http": ebu_proxy, "https": ebu_proxy},
                    )
                    result, report = uploader.check_compliance()
                    update_compliance_result(
                        request.node.nodeid, "Pass" if result else "Fail"
                    )
                    if result:
                        logger.info("PCAP compliance check passed")
                    else:
                        log_fail("PCAP compliance check failed")
                        logger.info(f"Compliance report: {report}")

                # Remove pcap file after upload to free up ramdisk space
                try:
                    capturer.host.connection.execute_command(
                        f"rm -f '{capturer.pcap_file}'"
                    )
                    logger.debug(f"Removed pcap file: {capturer.pcap_file}")
                except ConnectionCalledProcessError as e:
                    logger.warning(f"Failed to remove pcap file: {e}")

        # Always ensure netsniff-ng is stopped before fixture cleanup completes
        # This is critical because prepare_ramdisk unmount happens after this fixture
        # and will fail if netsniff-ng is still holding the pcap directory
        if capturer:
            capturer.stop()


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

    def fail_test(stage):
        logger.log(level=TEST_FAIL, msg=f"{stage} failed for {case_id}")
        os.chmod(logfile, 0o4755)
        return "Fail"

    if report["setup"].failed:
        result = fail_test("Setup")
    elif ("call" not in report) or report["call"].failed:
        result = fail_test("Test")
    elif report["call"].passed:
        compliance = get_compliance_result(case_id)
        if compliance is not None and compliance == "Fail":
            result = fail_test("Compliance")
        else:
            logger.log(level=TEST_PASS, msg=f"Test passed for {case_id}")
            os.chmod(logfile, 0o755)
            result = "Pass"
    else:
        logger.log(level=TEST_INFO, msg=f"Test skipped for {case_id}")
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


@pytest.fixture(scope="session", autouse=True)
def init_ip_address_pools(test_config: dict[Any, Any]) -> None:
    session_id = int(test_config["session_id"])
    ip_pools.init(session_id=session_id)
