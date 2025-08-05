# # SPDX-License-Identifier: BSD-3-Clause
# # Copyright 2024-2025 Intel Corporation
# # Media Communications Mesh
import datetime
import logging
import os
import shutil
import time
from typing import Dict

import pytest
from common.nicctl import Nicctl
from create_pcap_file.ramdisk import RamdiskPreparer
from mfd_common_libs.custom_logger import add_logging_level
from mfd_common_libs.log_levels import TEST_FAIL, TEST_INFO, TEST_PASS
from mtl_engine.const import LOG_FOLDER, TESTCMD_LVL
from mtl_engine.csv_report import (
    csv_add_test,
    csv_write_report,
    update_compliance_result,
)
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
def nic_port_list(hosts: dict, mtl_path) -> None:
    for host in hosts.values():
        nicctl = Nicctl(mtl_path, host)
        if int(host.network_interfaces[0].virtualization.get_current_vfs()) == 0:
            vfs = nicctl.create_vfs(host.network_interfaces[0].pci_address)
        vfs = nicctl.vfio_list()
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
    shutil.copy("pytest.log", f"{LOG_FOLDER}/latest/pytest.log")
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


@pytest.fixture(scope="session")
def build_openh264(hosts, test_config):
    """
    Build and install openh264 on all hosts in the specified directory.
    Only clones and builds if not already present.
    """
    openh264_dir = test_config.get("openh264_path", "/tmp/openh264")
    for host in hosts.values():
        conn = host.connection
        logger.info(f"[{host.name}] Building and installing openh264 in {openh264_dir}")
        # Ensure the parent directory exists
        conn.execute_command(f"mkdir -p {openh264_dir}")
        openh264_repo_dir = os.path.join(openh264_dir, "openh264")
        # Only clone if not present
        if (
            conn.execute_command(
                f"test -d {openh264_repo_dir} && echo 1 || echo 0"
            ).stdout.strip()
            == "1"
        ):
            logger.info(f"[{host.name}] openh264 repo exists, pulling latest changes")
            conn.execute_command(
                f"cd {openh264_repo_dir} && git fetch && git checkout openh264v2.4.0 && git pull"
            )
        else:
            logger.info(f"[{host.name}] Cloning openh264 repo")
            conn.execute_command(
                f"cd {openh264_dir} && git clone https://github.com/cisco/openh264.git"
            )
            conn.execute_command(
                f"cd {os.path.join(openh264_dir, 'openh264')} && git checkout openh264v2.4.0"
            )
        conn.execute_command(f"cd {openh264_repo_dir} && make -j $(nproc)")
        conn.execute_command(f"cd {openh264_repo_dir} && sudo make install")
        conn.execute_command("sudo ldconfig")
    yield


@pytest.fixture(scope="session")
def build_mtl_ffmpeg(hosts, test_config, build_openh264):
    """
    Build and install FFmpeg with the MTL plugin on all hosts.
    Removes any previous FFmpeg Plugin directory before proceeding.
    """
    ffmpeg_dir = test_config.get("ffmpeg_path", "/temp/ffmpeg")
    ffmpeg_version = str(test_config.get("ffmpeg_version", "7.0"))
    repo_dir = test_config.get("mtl_path", "/temp/Media-Transport-Library")
    ffmpeg_plugin_dir = os.path.join(repo_dir, "ecosystem", "ffmpeg_plugin")
    ffmpeg_clone_dir = os.path.join(ffmpeg_dir, "FFmpeg")

    for host in hosts.values():
        conn = host.connection
        # openh264 is built by the build_openh264 fixture

        # 1. Remove previous FFmpeg Plugin directory if it exists
        logger.info(
            f"[{host.name}] 1. Removing previous FFmpeg directory: {ffmpeg_clone_dir}"
        )
        conn.execute_command(f"rm -rf {ffmpeg_clone_dir}")

        # 2. Install required system packages
        required_packages = [
            "libfreetype6-dev",
            "libharfbuzz-dev",
            "libfontconfig1-dev",
            "tesseract-ocr",
        ]
        for pkg in required_packages:
            result = conn.execute_command(
                f"dpkg -s {pkg} > /dev/null 2>&1 && echo 1 || echo 0"
            )
            if result.stdout.strip() == "0":
                logger.info(f"[{host.name}] Installing missing package: {pkg}")
                conn.execute_command(f"sudo apt install -y {pkg}")
            else:
                logger.info(f"[{host.name}] Package already installed: {pkg}")

        # 3. Clone FFmpeg and checkout the specified version
        logger.info(
            f"[{host.name}] 3. Cloning FFmpeg and checking out version {ffmpeg_version}"
        )
        conn.execute_command(
            f"git clone https://github.com/FFmpeg/FFmpeg.git {ffmpeg_clone_dir}"
        )
        conn.execute_command(
            f"cd {ffmpeg_clone_dir} && git checkout release/{ffmpeg_version}"
        )

        # 4. Apply the build patch
        patch_dir = os.path.join(ffmpeg_plugin_dir, str(ffmpeg_version))
        logger.info(f"[{host.name}] 4. Applying build patches from {patch_dir}")
        for patch_file in sorted(os.listdir(patch_dir)):
            if patch_file.endswith(".patch"):
                conn.execute_command(
                    f"cd {ffmpeg_clone_dir} && git apply {os.path.join(patch_dir, patch_file)}"
                )

        # 5. Copy mtl in/out implementation code
        logger.info(f"[{host.name}] 5. Copying mtl in/out implementation code")
        for fname in os.listdir(ffmpeg_plugin_dir):
            if fname.startswith("mtl_") and (
                fname.endswith(".c") or fname.endswith(".h")
            ):
                conn.execute_command(
                    f"cp {os.path.join(ffmpeg_plugin_dir, fname)} {os.path.join(ffmpeg_clone_dir, 'libavdevice')}/"
                )

        # 6. Configure, build, and install FFmpeg
        logger.info(f"[{host.name}] 6. Configuring FFmpeg build")

        # Define configure options as a list:
        configure_options = [
            "--enable-shared",
            "--enable-mtl",
            "--enable-libfreetype",
            "--enable-libharfbuzz",
            "--enable-libfontconfig",
            "--disable-static",
            "--enable-nonfree",
            "--enable-pic",
            "--enable-gpl",
            "--enable-libopenh264",
            "--enable-encoder=libopenh264",
        ]

        # Join options into a single string
        configure_opts_str = " ".join(configure_options)

        # Use in command
        conn.execute_command(
            f"cd {ffmpeg_clone_dir} && ./configure {configure_opts_str}"
        )
        logger.info(f"[{host.name}] 7. Building FFmpeg")
        conn.execute_command(f"cd {ffmpeg_clone_dir} && make -j $(nproc)")
        logger.info(f"[{host.name}] 8. Installing FFmpeg")
        conn.execute_command(f"cd {ffmpeg_clone_dir} && sudo make install")
        logger.info(f"[{host.name}] 9. Running ldconfig")
        conn.execute_command("sudo ldconfig")

        # 9. Install Python packages
        logger.info(f"[{host.name}] 11. Installing Python packages")
        conn.execute_command(
            "python3 -m pip install opencv-python~=4.11.0 pytesseract~=0.3.13 matplotlib~=3.10.3"
        )

    yield ffmpeg_clone_dir  # Path to the built FFmpeg directory on each host
