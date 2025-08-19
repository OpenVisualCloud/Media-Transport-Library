# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import copy
import json
import logging
import os
import re
import time

from mfd_connect import SSHConnection

from mtl_engine.RxTxApp import prepare_netsniff, prepare_tcpdump

from . import rxtxapp_config
from .execute import log_fail, run

RXTXAPP_PATH = "./tests/tools/RxTxApp/build/RxTxApp"
logger = logging.getLogger(__name__)

ip_dict = dict(
    rx_interfaces="192.168.96.2",
    tx_interfaces="192.168.96.3",
    rx_sessions="239.168.85.20",
    tx_sessions="239.168.85.20",
)

ip_dict_rgb24_multiple = dict(
    p_sip_1="192.168.96.2",
    p_sip_2="192.168.96.3",
    p_tx_ip_1="239.168.108.202",
    p_tx_ip_2="239.168.108.203",
)

# Global variable to store timestamp for consistent logging
_log_timestamp = None


def get_case_id() -> str:
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    # Extract the test function name and parameters
    full_case = case_id[: case_id.rfind("(") - 1]
    # Get the test name after the last ::
    test_name = full_case.split("::")[-1]
    return test_name


def init_test_logging():
    """Initialize logging timestamp for the test"""
    global _log_timestamp
    _log_timestamp = time.strftime("%Y%m%d_%H%M%S")


def sanitize_filename(name: str) -> str:
    # Replace unsafe characters with underscores
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)


def log_to_file(message: str, host, build: str):
    """Log message to a file on the remote host"""
    global _log_timestamp

    # Initialize timestamp if not set
    if _log_timestamp is None:
        init_test_logging()

    test_name = sanitize_filename(get_case_id())
    log_file = f"{build}/tests/{test_name}_{_log_timestamp}_ffmpeg.log"

    remote_conn = host.connection
    f = remote_conn.path(log_file)

    # Ensure parent directory exists
    parent_dir = os.path.dirname(log_file)
    run(f"mkdir -p {parent_dir}", host=host)

    # Append to file with timestamp
    log_timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    log_entry = f"[{log_timestamp}] {message}\n"

    if f.exists():
        current_content = f.read_text()
        f.write_text(current_content + log_entry, encoding="utf-8")
    else:
        f.write_text(log_entry, encoding="utf-8")


def execute_test(
    test_time: int,
    build: str,
    host,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
    output_format: str,
    multiple_sessions: bool = False,
    tx_is_ffmpeg: bool = True,
    capture_cfg=None,
):
    # Initialize logging for this test
    init_test_logging()

    case_id = os.environ.get("PYTEST_CURRENT_TEST", "ffmpeg_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    nic_port_list = host.vfs
    video_size, fps = decode_video_format_16_9(video_format)
    match output_format:
        case "yuv":
            ffmpeg_rx_f_flag = "-f rawvideo"
        case "h264":
            ffmpeg_rx_f_flag = "-c:v libopenh264"
    if not multiple_sessions:
        output_files = create_empty_output_files(output_format, 1, host, build)
        rx_cmd = (
            f"ffmpeg -p_port {nic_port_list[0]} "
            f"-p_sip {ip_dict['rx_interfaces']} "
            f"-p_rx_ip {ip_dict['rx_sessions']} -udp_port 20000 "
            f"-payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i k "
            f"{ffmpeg_rx_f_flag} {output_files[0]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"ffmpeg -video_size {video_size} -f rawvideo "
                f"-pix_fmt yuv422p10le -i {video_url} "
                f"-filter:v fps={fps} -p_port {nic_port_list[1]} "
                f"-p_sip {ip_dict['tx_interfaces']} "
                f"-p_tx_ip {ip_dict['tx_sessions']} -udp_port 20000 "
                f"-payload_type 112 -f mtl_st20p -"
            )
        else:  # tx is rxtxapp
            tx_config_file = generate_rxtxapp_tx_config(
                nic_port_list[1], video_format, video_url, host, build
            )
            tx_cmd = f"{RXTXAPP_PATH} --config_file {tx_config_file}"
    else:  # multiple sessions
        output_files = create_empty_output_files(output_format, 2, host, build)
        rx_cmd = (
            f"ffmpeg -p_sip {ip_dict['rx_interfaces']} "
            f"-p_port {nic_port_list[0]} "
            f"-p_rx_ip {ip_dict['rx_sessions']} -udp_port 20000 "
            f"-payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 1 "
            f"-p_port {nic_port_list[0]} "
            f"-p_rx_ip {ip_dict['rx_sessions']} -udp_port 20002 "
            f"-payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 2 "
            f"-map 0:0 {ffmpeg_rx_f_flag} {output_files[0]} -y "
            f"-map 1:0 {ffmpeg_rx_f_flag} {output_files[1]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"ffmpeg -video_size {video_size} -f rawvideo "
                f"-pix_fmt yuv422p10le -i {video_url} "
                f"-filter:v fps={fps} -p_port {nic_port_list[1]} "
                f"-p_sip {ip_dict['tx_interfaces']} "
                f"-p_tx_ip {ip_dict['tx_sessions']} -udp_port 20000 "
                f"-payload_type 112 -f mtl_st20p -"
            )
        else:  # tx is rxtxapp
            tx_config_file = generate_rxtxapp_tx_config(
                nic_port_list[1], video_format, video_url, host, build, True
            )
            tx_cmd = f"{RXTXAPP_PATH} --config_file {tx_config_file}"

    logger.info(f"RX Command: {rx_cmd}")
    logger.info(f"TX Command: {tx_cmd}")
    log_to_file(f"RX Command: {rx_cmd}", host, build)
    log_to_file(f"TX Command: {tx_cmd}", host, build)

    rx_proc = None
    tx_proc = None
    tcpdump = prepare_tcpdump(capture_cfg, host)
    netsniff = prepare_netsniff(
        capture_cfg,
        host,
        src_ip = str(ip_dict['tx_interfaces']),
        dst_ip = str(ip_dict['tx_sessions'])
    )
    try:
        # Start RX pipeline first
        logger.info("Starting RX pipeline...")
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        time.sleep(2)

        # Start TX pipeline
        logger.info("Starting TX pipeline...")
        tx_proc = run(
            tx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        # Start packet capture when pipelines are running
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))
        if netsniff:
            logger.info("Starting netsniff-ng capture...")
            netsniff.capture(capture_time=capture_cfg.get("capture_time", test_time))

        # Let the test run for the specified duration
        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)

        logger.info("Terminating processes...")
        if tx_proc:
            try:
                tx_proc.terminate()
            except Exception:
                pass
        if rx_proc:
            try:
                rx_proc.terminate()
            except Exception:
                pass
        # Wait a bit for termination
        time.sleep(2)
        # Get output after processes have been terminated
        try:
            if rx_proc and hasattr(rx_proc, "stdout_text"):
                log_to_file(f"RX Output: {rx_proc.stdout_text}", host, build)
        except Exception:
            logger.info("Could not retrieve RX output")
        try:
            if tx_proc and hasattr(tx_proc, "stdout_text"):
                log_to_file(f"TX Output: {tx_proc.stdout_text}", host, build)
        except Exception:
            logger.info("Could not retrieve TX output")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        if tx_proc:
            try:
                tx_proc.terminate()
            except Exception:
                pass
        if rx_proc:
            try:
                rx_proc.terminate()
            except Exception:
                pass
        raise
    finally:
        # Ensure processes are terminated with force kill if needed
        if tx_proc:
            try:
                tx_proc.terminate()
                tx_proc.wait(timeout=5)
            except Exception:
                try:
                    # Force kill if terminate didn't work
                    tx_proc.kill()
                    tx_proc.wait(timeout=5)
                except Exception:
                    pass
        if rx_proc:
            try:
                rx_proc.terminate()
                rx_proc.wait(timeout=5)
            except Exception:
                try:
                    # Force kill if terminate didn't work
                    rx_proc.kill()
                    rx_proc.wait(timeout=5)
                except Exception:
                    pass
        if tcpdump:
            tcpdump.stop()
        if netsniff:
            netsniff.stop()
    passed = False
    match output_format:
        case "yuv":
            passed = check_output_video_yuv(output_files[0], host, build, video_url)
        case "h264":
            passed = check_output_video_h264(
                output_files[0], video_size, host, build, video_url
            )
    # Clean up output files after validation
    try:
        for output_file in output_files:
            run(f"rm -f {output_file}", host=host)
            logger.info(f"Removed output file: {output_file}")
    except Exception as e:
        logger.info(f"Could not remove output files: {e}")
    if not passed:
        log_fail("test failed")
    return passed


def execute_test_rgb24(
    test_time: int,
    build: str,
    host,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
    capture_cfg=None,
):
    # Initialize logging for this test
    init_test_logging()
    nic_port_list = host.vfs
    video_size, fps = decode_video_format_16_9(video_format)
    logger.info(f"Creating RX config for RGB24 test with video_format: {video_format}")
    log_to_file(
        f"Creating RX config for RGB24 test with video_format: {video_format}",
        host,
        build,
    )
    try:
        rx_config_file = generate_rxtxapp_rx_config(
            nic_port_list[0], video_format, host, build
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
        log_to_file(
            f"Successfully created RX config file: {rx_config_file}", host, build
        )
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        log_to_file(f"Failed to create RX config file: {e}", host, build)
        return False
    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_cmd = (
        f"ffmpeg -stream_loop -1 -video_size {video_size} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url} -filter:v fps={fps} -p_port {nic_port_list[1]} "
        f"-p_sip {ip_dict['tx_interfaces']} -p_tx_ip {ip_dict['tx_sessions']} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    logger.info(f"RX Command: {rx_cmd}")
    logger.info(f"TX Command: {tx_cmd}")
    log_to_file(f"RX Command: {rx_cmd}", host, build)
    log_to_file(f"TX Command: {tx_cmd}", host, build)

    rx_proc = None
    tx_proc = None
    tcpdump = prepare_tcpdump(capture_cfg, host)
    netsniff = prepare_netsniff(
        capture_cfg,
        host,
        src_ip = str(ip_dict['tx_interfaces']),
        dst_ip = str(ip_dict['tx_sessions'])
    )

    try:
        # Start RX pipeline first
        logger.info("Starting RX pipeline...")
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        time.sleep(5)
        # Start TX pipeline
        logger.info("Starting TX pipeline...")
        tx_proc = run(
            tx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        # Start packet capture when pipelines are running
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))
        if netsniff:
            logger.info("Starting netsniff-ng capture...")
            netsniff.capture(capture_time=capture_cfg.get("capture_time", test_time))

        logger.info(
            f"Waiting for RX process to complete (test_time: {test_time} seconds)..."
        )
        rx_proc.wait()
        logger.info("RX process completed")

        # Terminate TX process after RX completes
        logger.info("Terminating TX process...")
        if tx_proc:
            try:
                tx_proc.terminate()
                tx_proc.wait(timeout=5)
                logger.info("TX process terminated successfully")
            except Exception:
                try:
                    tx_proc.kill()
                    tx_proc.wait(timeout=5)
                    logger.info("TX process killed")
                except Exception:
                    logger.info("Could not terminate TX process")
        rx_output = ""
        try:
            if rx_proc and hasattr(rx_proc, "stdout_text"):
                rx_output = rx_proc.stdout_text
                log_to_file(f"RX Output: {rx_output}", host, build)
                logger.info("RX output captured successfully")
            else:
                logger.info("Could not retrieve RX output")
                log_to_file("Could not retrieve RX output", host, build)
        except Exception as e:
            logger.info(f"Error retrieving RX output: {e}")
            log_to_file(f"Error retrieving RX output: {e}", host, build)
        try:
            if tx_proc and hasattr(tx_proc, "stdout_text"):
                log_to_file(f"TX Output: {tx_proc.stdout_text}", host, build)
                logger.info("TX output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving TX output: {e}")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        if tx_proc:
            try:
                tx_proc.terminate()
            except Exception:
                pass
        if rx_proc:
            try:
                rx_proc.terminate()
            except Exception:
                pass
        raise
    finally:
        # Final cleanup - ensure processes are terminated
        if tx_proc:
            try:
                tx_proc.terminate()
                tx_proc.wait(timeout=3)
            except Exception:
                try:
                    tx_proc.kill()
                    tx_proc.wait(timeout=3)
                except Exception:
                    pass
        if rx_proc:
            try:
                rx_proc.terminate()
                rx_proc.wait(timeout=3)
            except Exception:
                try:
                    rx_proc.kill()
                    rx_proc.wait(timeout=3)
                except Exception:
                    pass
        if tcpdump:
            tcpdump.stop()
        if netsniff:
            netsniff.stop()
    if not check_output_rgb24(rx_output, 1):
        log_fail("rx video sessions failed")
        return False
    time.sleep(5)
    return True


def execute_test_rgb24_multiple(
    test_time: int,
    build: str,
    nic_port_list: list,
    type_: str,
    video_format_list: list,
    pg_format: str,
    video_url_list: list,
    host,
    capture_cfg=None,
):
    # Initialize logging for this test
    init_test_logging()
    video_size_1, fps_1 = decode_video_format_16_9(video_format_list[0])
    video_size_2, fps_2 = decode_video_format_16_9(video_format_list[1])
    logger.info(
        f"Creating RX config for RGB24 multiple test with video_formats: {video_format_list}"
    )
    log_to_file(
        f"Creating RX config for RGB24 multiple test with video_formats: {video_format_list}",
        host,
        build,
    )
    try:
        rx_config_file = generate_rxtxapp_rx_config_multiple(
            nic_port_list[:2], video_format_list, host, build, True
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
        log_to_file(
            f"Successfully created RX config file: {rx_config_file}", host, build
        )
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        log_to_file(f"Failed to create RX config file: {e}", host, build)
        return False
    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_1_cmd = (
        f"ffmpeg -stream_loop -1 -video_size {video_size_1} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url_list[0]} -filter:v fps={fps_1} -p_port {nic_port_list[2]} "
        f"-p_sip {ip_dict_rgb24_multiple['p_sip_1']} "
        f"-p_tx_ip {ip_dict_rgb24_multiple['p_tx_ip_1']} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )
    tx_2_cmd = (
        f"ffmpeg -stream_loop -1 -video_size {video_size_2} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url_list[1]} -filter:v fps={fps_2} -p_port {nic_port_list[3]} "
        f"-p_sip {ip_dict_rgb24_multiple['p_sip_2']} "
        f"-p_tx_ip {ip_dict_rgb24_multiple['p_tx_ip_2']} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    logger.info(f"RX Command: {rx_cmd}")
    logger.info(f"TX1 Command: {tx_1_cmd}")
    logger.info(f"TX2 Command: {tx_2_cmd}")
    log_to_file(f"RX Command: {rx_cmd}", host, build)
    log_to_file(f"TX1 Command: {tx_1_cmd}", host, build)
    log_to_file(f"TX2 Command: {tx_2_cmd}", host, build)

    rx_proc = None
    tx_1_proc = None
    tx_2_proc = None
    tcpdump = prepare_tcpdump(capture_cfg, host)
    netsniff = prepare_netsniff(
        capture_cfg,
        host,
        src_ip = f"({ip_dict_rgb24_multiple['p_sip_1']} or {ip_dict_rgb24_multiple['p_sip_2']})",
        dst_ip = f"({ip_dict_rgb24_multiple['p_tx_ip_1']} or {ip_dict_rgb24_multiple['p_tx_ip_2']})"
    )

    try:
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        time.sleep(5)
        # Start TX pipelines
        logger.info("Starting TX pipelines...")
        tx_1_proc = run(
            tx_1_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        tx_2_proc = run(
            tx_2_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
            enable_sudo=True,
        )
        # Start packet capture when pipelines are running
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))
        if netsniff:
            logger.info("Starting netsniff-ng capture...")
            netsniff.capture(capture_time=capture_cfg.get("capture_time", test_time))

        logger.info(f"Waiting for RX process (test_time: {test_time} seconds)...")
        rx_proc.wait()
        logger.info("RX process completed")

        # Terminate TX processes after RX completes
        logger.info("Terminating TX processes...")
        for proc in [tx_1_proc, tx_2_proc]:
            if proc:
                try:
                    proc.terminate()
                    proc.wait(timeout=5)
                    logger.info("TX process terminated successfully")
                except Exception:
                    try:
                        proc.kill()
                        proc.wait(timeout=5)
                        logger.info("TX process killed")
                    except Exception:
                        logger.info("Could not terminate TX process")
        rx_output = ""
        try:
            if rx_proc and hasattr(rx_proc, "stdout_text"):
                rx_output = rx_proc.stdout_text
                log_to_file(f"RX Output: {rx_output}", host, build)
                logger.info("RX output captured successfully")
            else:
                logger.info("Could not retrieve RX output")
                log_to_file("Could not retrieve RX output", host, build)
        except Exception as e:
            logger.info(f"Error retrieving RX output: {e}")
            log_to_file(f"Error retrieving RX output: {e}", host, build)
        try:
            if tx_1_proc and hasattr(tx_1_proc, "stdout_text"):
                log_to_file(f"TX1 Output: {tx_1_proc.stdout_text}", host, build)
                logger.info("TX1 output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving TX1 output: {e}")
        try:
            if tx_2_proc and hasattr(tx_2_proc, "stdout_text"):
                log_to_file(f"TX2 Output: {tx_2_proc.stdout_text}", host, build)
                logger.info("TX2 output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving TX2 output: {e}")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        for proc in [tx_1_proc, tx_2_proc, rx_proc]:
            if proc:
                try:
                    proc.terminate()
                except Exception:
                    pass
        raise
    finally:
        # Final cleanup - ensure processes are terminated
        for proc in [tx_1_proc, tx_2_proc, rx_proc]:
            if proc:
                try:
                    proc.terminate()
                    proc.wait(timeout=3)
                except Exception:
                    try:
                        proc.kill()
                        proc.wait(timeout=3)
                    except Exception:
                        pass
        if tcpdump:
            tcpdump.stop()
        if netsniff:
            netsniff.stop()
    if not check_output_rgb24(rx_output, 2):
        log_fail("rx video session failed")
        return False
    time.sleep(5)
    return True


def check_output_video_yuv(output_file: str, host, build: str, input_file: str):
    # Log input file size
    try:
        input_stat_proc = run(f"stat -c '%s' {input_file}", host=host)
        if input_stat_proc.return_code == 0:
            input_file_size = int(input_stat_proc.stdout_text.strip())
            logger.info(f"Input file size: {input_file_size} bytes for {input_file}")
            log_to_file(
                f"Input file size: {input_file_size} bytes for {input_file}",
                host,
                build,
            )
        else:
            logger.info(f"Could not get input file size for {input_file}")
            log_to_file(f"Could not get input file size for {input_file}", host, build)
    except Exception as e:
        logger.info(f"Error checking input file size: {e}")
        log_to_file(f"Error checking input file size: {e}", host, build)

    # Use run() to check output file size
    stat_proc = run(f"stat -c '%s' {output_file}", host=host)

    if stat_proc.return_code == 0:
        output_file_size = int(stat_proc.stdout_text.strip())
        logger.info(f"Output file size: {output_file_size} bytes for {output_file}")
        log_to_file(
            f"Output file size: {output_file_size} bytes for {output_file}", host, build
        )
        result = output_file_size > 0
        logger.info(f"YUV check result: {result}")
        log_to_file(f"YUV check result: {result}", host, build)
        return result
    else:
        logger.info(f"Could not get output file size for {output_file}")
        log_to_file(f"Could not get output file size for {output_file}", host, build)
        return False


def check_output_video_h264(
    output_file: str, video_size: str, host, build: str, input_file: str
):
    # Log input file size
    try:
        input_stat_proc = run(f"stat -c '%s' {input_file}", host=host)
        if input_stat_proc.return_code == 0:
            input_file_size = int(input_stat_proc.stdout_text.strip())
            logger.info(f"Input file size: {input_file_size} bytes for {input_file}")
            log_to_file(
                f"Input file size: {input_file_size} bytes for {input_file}",
                host,
                build,
            )
        else:
            logger.info(f"Could not get input file size for {input_file}")
            log_to_file(f"Could not get input file size for {input_file}", host, build)
    except Exception as e:
        logger.info(f"Error checking input file size: {e}")
        log_to_file(f"Error checking input file size: {e}", host, build)

    # Log output file size first
    try:
        stat_proc = run(f"stat -c '%s' {output_file}", host=host)
        if stat_proc.return_code == 0:
            output_file_size = int(stat_proc.stdout_text.strip())
            logger.info(f"Output file size: {output_file_size} bytes for {output_file}")
            log_to_file(
                f"Output file size: {output_file_size} bytes for {output_file}",
                host,
                build,
            )
        else:
            logger.info(f"Could not get output file size for {output_file}")
            log_to_file(
                f"Could not get output file size for {output_file}", host, build
            )
    except Exception as e:
        logger.info(f"Error checking output file size: {e}")
        log_to_file(f"Error checking output file size: {e}", host, build)

    code_name_pattern = r"codec_name=([^\n]+)"
    width_pattern = r"width=(\d+)"
    height_pattern = r"height=(\d+)"

    ffprobe_proc = run(
        f"ffprobe -v error -show_format -show_streams {output_file}", host=host
    )

    codec_name_match = re.search(code_name_pattern, ffprobe_proc.stdout_text)
    width_match = re.search(width_pattern, ffprobe_proc.stdout_text)
    height_match = re.search(height_pattern, ffprobe_proc.stdout_text)

    if codec_name_match and width_match and height_match:
        codec_name = codec_name_match.group(1)
        width = width_match.group(1)
        height = height_match.group(1)

        result = codec_name == "h264" and f"{width}x{height}" == video_size
        logger.info(
            f"H264 check result: {result} (codec: {codec_name}, size: {width}x{height})"
        )
        log_to_file(
            f"H264 check result: {result} (codec: {codec_name}, size: {width}x{height})",
            host,
            build,
        )
        return result
    else:
        logger.info("H264 check failed")
        log_to_file("H264 check failed", host, build)
        return False


def check_output_rgb24(rx_output: str, number_of_sessions: int):
    lines = rx_output.splitlines()
    ok_cnt = 0

    for line in lines:
        if "app_rx_st20p_result" in line and "OK" in line:
            ok_cnt += 1

    return ok_cnt == number_of_sessions


def create_empty_output_files(
    output_format: str, number_of_files: int = 1, host=None, build: str = ""
) -> list:
    output_files = []

    # Create a timestamp for uniqueness
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    test_name = sanitize_filename(get_case_id())

    for i in range(number_of_files):
        output_file = f"{build}/tests/{test_name}_{timestamp}_out_{i}.{output_format}"
        output_files.append(output_file)

        remote_conn = host.connection
        f = remote_conn.path(output_file)
        f.touch()

    return output_files


def decode_video_format_16_9(video_format: str) -> tuple:
    pattern = r"i(\d+)([ip])(\d+)"
    match = re.search(pattern, video_format)

    if match:
        height = match.group(1)
        width = int(height) * (16 / 9)
        fps = match.group(3)
        return f"{int(width)}x{height}", int(fps)
    else:
        log_fail("Invalid video format")
        return None


def generate_rxtxapp_rx_config(
    nic_port: str,
    video_format: str,
    host,
    build: str,
    multiple_sessions: bool = False,
) -> str:
    logger.info(
        f"Generating RX ST20P config for nic_port: {nic_port}, video_format: {video_format}"
    )

    try:
        config = copy.deepcopy(rxtxapp_config.config_empty_rx)
        config["interfaces"][0]["name"] = nic_port
        config["interfaces"][0]["ip"] = ip_dict["rx_interfaces"]
        config["rx_sessions"][0]["ip"][0] = ip_dict["rx_sessions"]

        width, height, fps = decode_video_format_to_st20p(video_format)
        logger.info(f"Decoded video format: width={width}, height={height}, fps={fps}")

        rx_session = copy.deepcopy(rxtxapp_config.config_rx_st20p_session)
        config["rx_sessions"][0]["st20p"].append(rx_session)
        config["rx_sessions"][0]["st20p"][0]["width"] = width
        config["rx_sessions"][0]["st20p"][0]["height"] = height
        config["rx_sessions"][0]["st20p"][0]["fps"] = fps
        config["rx_sessions"][0]["st20p"][0]["transport_format"] = "RGB_8bit"
        config["rx_sessions"][0]["st20p"][0]["output_format"] = "RGB8"

        if multiple_sessions:
            rx_session = copy.deepcopy(rxtxapp_config.config_rx_st20p_session)
            config["rx_sessions"][0]["st20p"].append(rx_session)
            config["rx_sessions"][0]["st20p"][1]["start_port"] = 20002
            config["rx_sessions"][0]["st20p"][1]["width"] = width
            config["rx_sessions"][0]["st20p"][1]["height"] = height
            config["rx_sessions"][0]["st20p"][1]["fps"] = fps
            config["rx_sessions"][0]["st20p"][1]["transport_format"] = "RGB_8bit"
            config["rx_sessions"][0]["st20p"][1]["output_format"] = "RGB8"

        test_name = sanitize_filename(get_case_id())
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        config_file = f"{build}/tests/{test_name}_{timestamp}_rx.json"

        logger.info(f"Writing config file to: {config_file}")

        config_json = json.dumps(config, indent=4)
        remote_conn = host.connection
        f = remote_conn.path(config_file)
        if isinstance(remote_conn, SSHConnection):
            config_json = config_json.replace('"', '\\"')
        f.write_text(config_json, encoding="utf-8")

        logger.info("Config file written successfully")
        log_to_file(f"Generated RX config file: {config_file}", host, build)

        return config_file

    except Exception as e:
        log_fail(f"Error generating RX ST20P config: {e}")
        raise


def generate_rxtxapp_rx_config_multiple(
    nic_port_list: list,
    video_format_list: list,
    host,
    build: str,
    multiple_sessions: bool = False,
) -> str:
    logger.info(
        f"Generating RX ST20P multiple config for nic_ports: {nic_port_list}, "
        f"video_formats: {video_format_list}"
    )

    try:
        config = copy.deepcopy(rxtxapp_config.config_empty_rx_rgb24_multiple)
        config["interfaces"][0]["name"] = nic_port_list[0]
        config["interfaces"][0]["ip"] = ip_dict_rgb24_multiple["p_sip_1"]
        config["rx_sessions"][0]["ip"][0] = ip_dict_rgb24_multiple["p_tx_ip_1"]

        config["interfaces"][1]["name"] = nic_port_list[1]
        config["interfaces"][1]["ip"] = ip_dict_rgb24_multiple["p_sip_2"]
        config["rx_sessions"][1]["ip"][0] = ip_dict_rgb24_multiple["p_tx_ip_2"]

        width_1, height_1, fps_1 = decode_video_format_to_st20p(video_format_list[0])
        width_2, height_2, fps_2 = decode_video_format_to_st20p(video_format_list[1])

        logger.info(f"Session 1: width={width_1}, height={height_1}, fps={fps_1}")
        logger.info(f"Session 2: width={width_2}, height={height_2}, fps={fps_2}")

        rx_session_1 = copy.deepcopy(rxtxapp_config.config_rx_st20p_session)
        config["rx_sessions"][0]["st20p"].append(rx_session_1)
        config["rx_sessions"][0]["st20p"][0]["width"] = width_1
        config["rx_sessions"][0]["st20p"][0]["height"] = height_1
        config["rx_sessions"][0]["st20p"][0]["fps"] = fps_1
        config["rx_sessions"][0]["st20p"][0]["transport_format"] = "RGB_8bit"
        config["rx_sessions"][0]["st20p"][0]["output_format"] = "RGB8"

        rx_session_2 = copy.deepcopy(rxtxapp_config.config_rx_st20p_session)
        config["rx_sessions"][1]["st20p"].append(rx_session_2)
        config["rx_sessions"][1]["st20p"][0]["width"] = width_2
        config["rx_sessions"][1]["st20p"][0]["height"] = height_2
        config["rx_sessions"][1]["st20p"][0]["fps"] = fps_2
        config["rx_sessions"][1]["st20p"][0]["transport_format"] = "RGB_8bit"
        config["rx_sessions"][1]["st20p"][0]["output_format"] = "RGB8"

        test_name = sanitize_filename(get_case_id())
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        config_file = f"{build}/tests/{test_name}_{timestamp}_rx.json"

        logger.info(f"Writing multiple config file to: {config_file}")

        config_json = json.dumps(config, indent=4)
        remote_conn = host.connection
        f = remote_conn.path(config_file)
        if isinstance(remote_conn, SSHConnection):
            config_json = config_json.replace('"', '\\"')
        f.write_text(config_json, encoding="utf-8")

        logger.info("Multiple config file written successfully")
        log_to_file(f"Generated RX multiple config file: {config_file}", host, build)

        return config_file

    except Exception as e:
        log_fail(f"Error generating RX ST20P multiple config: {e}")
        raise


def generate_rxtxapp_tx_config(
    nic_port: str,
    video_format: str,
    video_url: str,
    host,
    build: str,
    multiple_sessions: bool = False,
) -> str:
    logger.info(
        f"Generating TX ST20P config for nic_port: {nic_port}, video_format: {video_format}"
    )

    try:
        config = copy.deepcopy(rxtxapp_config.config_empty_tx)
        config["interfaces"][0]["name"] = nic_port
        config["interfaces"][0]["ip"] = ip_dict["tx_interfaces"]
        config["tx_sessions"][0]["dip"][0] = ip_dict["tx_sessions"]

        width, height, fps = decode_video_format_to_st20p(video_format)
        logger.info(f"Decoded video format: width={width}, height={height}, fps={fps}")

        tx_session = copy.deepcopy(rxtxapp_config.config_tx_st20p_session)
        config["tx_sessions"][0]["st20p"].append(tx_session)
        config["tx_sessions"][0]["st20p"][0]["width"] = width
        config["tx_sessions"][0]["st20p"][0]["height"] = height
        config["tx_sessions"][0]["st20p"][0]["fps"] = fps
        config["tx_sessions"][0]["st20p"][0]["transport_format"] = "YUV_422_10bit"
        config["tx_sessions"][0]["st20p"][0]["input_format"] = "YUV422PLANAR10LE"
        config["tx_sessions"][0]["st20p"][0]["st20p_url"] = video_url

        if multiple_sessions:
            tx_session = copy.deepcopy(rxtxapp_config.config_tx_st20p_session)
            config["tx_sessions"][0]["st20p"].append(tx_session)
            config["tx_sessions"][0]["st20p"][1]["start_port"] = 20002
            config["tx_sessions"][0]["st20p"][1]["width"] = width
            config["tx_sessions"][0]["st20p"][1]["height"] = height
            config["tx_sessions"][0]["st20p"][1]["fps"] = fps
            config["tx_sessions"][0]["st20p"][1]["transport_format"] = "YUV_422_10bit"
            config["tx_sessions"][0]["st20p"][1]["input_format"] = "YUV422PLANAR10LE"
            config["tx_sessions"][0]["st20p"][1]["st20p_url"] = video_url

        test_name = sanitize_filename(get_case_id())
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        config_file = f"{build}/tests/{test_name}_{timestamp}_tx.json"

        logger.info(f"Writing TX config file to: {config_file}")

        config_json = json.dumps(config, indent=4)
        remote_conn = host.connection
        f = remote_conn.path(config_file)
        if isinstance(remote_conn, SSHConnection):
            config_json = config_json.replace('"', '\\"')
        f.write_text(config_json, encoding="utf-8")

        logger.info("TX Config file written successfully")
        log_to_file(f"Generated TX config file: {config_file}", host, build)

        return config_file

    except Exception as e:
        log_fail(f"Error generating TX ST20P config: {e}")
        raise


def decode_video_format_to_st20p(video_format: str) -> tuple:
    """Convert video format string to st20p parameters (width, height, fps)"""
    logger.info(f"Decoding video format: {video_format}")

    pattern = r"i(\d+)([ip])(\d+)"
    match = re.search(pattern, video_format)

    if match:
        height = int(match.group(1))
        width = int(height * (16 / 9))
        fps_num = int(match.group(3))
        interlaced = match.group(2) == "i"

        # Convert to st20p fps format
        if interlaced:
            fps = f"i{fps_num}"
        else:
            fps = f"p{fps_num}"

        logger.info(
            f"Decoded: width={width}, height={height}, fps={fps}, interlaced={interlaced}"
        )
        return width, height, fps
    else:
        log_fail(f"Invalid video format: {video_format}")
        return None


def execute_dual_test(
    test_time: int,
    build: str,
    tx_host,
    rx_host,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
    output_format: str,
    multiple_sessions: bool = False,
    tx_is_ffmpeg: bool = True,
    capture_cfg=None,
):
    # Initialize logging for this test
    init_test_logging()

    case_id = os.environ.get("PYTEST_CURRENT_TEST", "ffmpeg_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    tx_nic_port_list = tx_host.vfs
    rx_nic_port_list = rx_host.vfs
    video_size, fps = decode_video_format_16_9(video_format)
    match output_format:
        case "yuv":
            ffmpeg_rx_f_flag = "-f rawvideo"
        case "h264":
            ffmpeg_rx_f_flag = "-c:v libopenh264"
    if not multiple_sessions:
        output_files = create_empty_output_files(output_format, 1, rx_host, build)
        rx_cmd = (
            f"ffmpeg -p_port {rx_nic_port_list[0]} -p_sip {ip_dict['rx_interfaces']} "
            f"-p_rx_ip {ip_dict['rx_sessions']} -udp_port 20000 -payload_type 112 "
            f"-fps {fps} -pix_fmt yuv422p10le -video_size {video_size} "
            f"-f mtl_st20p -i k {ffmpeg_rx_f_flag} {output_files[0]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"ffmpeg -video_size {video_size} -f rawvideo -pix_fmt yuv422p10le "
                f"-i {video_url} -filter:v fps={fps} -p_port {tx_nic_port_list[0]} "
                f"-p_sip {ip_dict['tx_interfaces']} -p_tx_ip {ip_dict['tx_sessions']} "
                f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
            )
        else:  # tx is rxtxapp
            tx_config_file = generate_rxtxapp_tx_config(
                tx_nic_port_list[0], video_format, video_url, tx_host, build
            )
            tx_cmd = f"{RXTXAPP_PATH} --config_file {tx_config_file}"
    else:  # multiple sessions
        output_files = create_empty_output_files(output_format, 2, rx_host, build)
        rx_cmd = (
            f"ffmpeg -p_sip {ip_dict['rx_interfaces']} "
            f"-p_port {rx_nic_port_list[0]} -p_rx_ip {ip_dict['rx_sessions']} "
            f"-udp_port 20000 -payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 1 "
            f"-p_port {rx_nic_port_list[0]} -p_rx_ip {ip_dict['rx_sessions']} "
            f"-udp_port 20002 -payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 2 "
            f"-map 0:0 {ffmpeg_rx_f_flag} {output_files[0]} -y "
            f"-map 1:0 {ffmpeg_rx_f_flag} {output_files[1]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"ffmpeg -video_size {video_size} -f rawvideo -pix_fmt yuv422p10le "
                f"-i {video_url} -filter:v fps={fps} -p_port {tx_nic_port_list[0]} "
                f"-p_sip {ip_dict['tx_interfaces']} -p_tx_ip {ip_dict['tx_sessions']} "
                f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
            )
        else:  # tx is rxtxapp
            tx_config_file = generate_rxtxapp_tx_config(
                tx_nic_port_list[0], video_format, video_url, tx_host, build, True
            )
            tx_cmd = f"{RXTXAPP_PATH} --config_file {tx_config_file}"

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")
    logger.info(f"RX Command: {rx_cmd}")
    logger.info(f"TX Command: {tx_cmd}")
    log_to_file(f"TX Host: {tx_host}", rx_host, build)
    log_to_file(f"RX Host: {rx_host}", rx_host, build)
    log_to_file(f"RX Command: {rx_cmd}", rx_host, build)
    log_to_file(f"TX Command: {tx_cmd}", tx_host, build)

    rx_proc = None
    tx_proc = None
    # Use RX host for tcpdump capture
    tcpdump = prepare_tcpdump(capture_cfg, rx_host)
    netsniff = prepare_netsniff(
        capture_cfg,
        rx_host,
        src_ip = str(ip_dict['tx_interfaces']),
        dst_ip = str(ip_dict['tx_sessions'])
    )

    try:
        # Start RX pipeline first on RX host
        logger.info("Starting RX pipeline on RX host...")
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=rx_host,
            background=True,
            enable_sudo=True,
        )
        time.sleep(2)

        # Start TX pipeline on TX host
        logger.info("Starting TX pipeline on TX host...")
        tx_proc = run(
            tx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=tx_host,
            background=True,
            enable_sudo=True,
        )
        # Start tcpdump after pipelines are running
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))
        if netsniff:
            logger.info("Starting netsniff capture...")
            netsniff.capture(capture_time=capture_cfg.get("capture_time", test_time))

        # Let the test run for the specified duration
        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)

        logger.info("Terminating processes...")
        if tx_proc:
            try:
                tx_proc.terminate()
            except Exception:
                pass
        if rx_proc:
            try:
                rx_proc.terminate()
            except Exception:
                pass
        # Wait a bit for termination
        time.sleep(2)
        # Get output after processes have been terminated
        try:
            rx_output = f"RX Output:\n{rx_proc.stdout_text}"
            log_to_file(rx_output, rx_host, build)
        except Exception:
            logger.info("Could not retrieve RX output")
        try:
            tx_output = f"TX Output:\n{tx_proc.stdout_text}"
            log_to_file(tx_output, tx_host, build)
        except Exception:
            logger.info("Could not retrieve TX output")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        if tx_proc:
            try:
                tx_proc.terminate()
            except Exception:
                pass
        if rx_proc:
            try:
                rx_proc.terminate()
            except Exception:
                pass
        raise
    finally:
        # Ensure processes are terminated with force kill if needed
        if tx_proc:
            try:
                tx_proc.terminate()
                tx_proc.wait(timeout=5)
            except Exception:
                try:
                    # Force kill if terminate didn't work
                    tx_proc.kill()
                    tx_proc.wait(timeout=5)
                except Exception:
                    pass
        if rx_proc:
            try:
                rx_proc.terminate()
                rx_proc.wait(timeout=5)
            except Exception:
                try:
                    # Force kill if terminate didn't work
                    rx_proc.kill()
                    rx_proc.wait(timeout=5)
                except Exception:
                    pass
        if tcpdump:
            tcpdump.stop()
        if netsniff:
            netsniff.stop()
    passed = False
    match output_format:
        case "yuv":
            passed = check_output_video_yuv(output_files[0], rx_host, build, video_url)
        case "h264":
            passed = check_output_video_h264(
                output_files[0], video_size, rx_host, build, video_url
            )
    # Clean up output files after validation
    try:
        for output_file in output_files:
            run(f"rm -f {output_file}", host=rx_host)
            logger.info(f"Removed output file: {output_file}")
    except Exception as e:
        logger.info(f"Could not remove output files: {e}")
    if not passed:
        log_fail("test failed")
    return passed


def execute_dual_test_rgb24(
    test_time: int,
    build: str,
    tx_host,
    rx_host,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
    capture_cfg=None,
):
    # Initialize logging for this test
    init_test_logging()

    # Use separate NIC port lists for TX and RX hosts
    tx_nic_port_list = tx_host.vfs
    rx_nic_port_list = rx_host.vfs
    video_size, fps = decode_video_format_16_9(video_format)

    logger.info(
        f"Creating RX config for RGB24 dual test with video_format: {video_format}"
    )
    log_to_file(
        f"Creating RX config for RGB24 dual test with video_format: {video_format}",
        rx_host,
        build,
    )
    try:
        rx_config_file = generate_rxtxapp_rx_config(
            rx_nic_port_list[0], video_format, rx_host, build
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
        log_to_file(
            f"Successfully created RX config file: {rx_config_file}", rx_host, build
        )
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        log_to_file(f"Failed to create RX config file: {e}", rx_host, build)
        return False

    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_cmd = (
        f"ffmpeg -stream_loop -1 -video_size {video_size} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url} -filter:v fps={fps} -p_port {tx_nic_port_list[0]} "
        f"-p_sip {ip_dict['tx_interfaces']} -p_tx_ip {ip_dict['tx_sessions']} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")
    logger.info(f"RX Command: {rx_cmd}")
    logger.info(f"TX Command: {tx_cmd}")
    log_to_file(f"TX Host: {tx_host}", rx_host, build)
    log_to_file(f"RX Host: {rx_host}", rx_host, build)
    log_to_file(f"RX Command: {rx_cmd}", rx_host, build)
    log_to_file(f"TX Command: {tx_cmd}", tx_host, build)

    rx_proc = None
    tx_proc = None
    # Use RX host for tcpdump capture
    tcpdump = prepare_tcpdump(capture_cfg, rx_host)

    try:
        # Start RX pipeline first on RX host
        logger.info("Starting RX pipeline on RX host...")
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=rx_host,
            background=True,
            enable_sudo=True,
        )
        time.sleep(5)

        # Start TX pipeline on TX host
        logger.info("Starting TX pipeline on TX host...")
        tx_proc = run(
            tx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=tx_host,
            background=True,
            enable_sudo=True,
        )

        # Start tcpdump after pipelines are running
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))

        logger.info(
            f"Waiting for RX process to complete (test_time: {test_time} seconds)..."
        )
        rx_proc.wait()
        logger.info("RX process completed")

        # Terminate TX process after RX completes
        logger.info("Terminating TX process...")
        if tx_proc:
            try:
                tx_proc.terminate()
                tx_proc.wait(timeout=5)
                logger.info("TX process terminated successfully")
            except Exception:
                try:
                    tx_proc.kill()
                    tx_proc.wait(timeout=5)
                    logger.info("TX process killed")
                except Exception:
                    logger.info("Could not terminate TX process")

        rx_output = ""
        try:
            rx_output = rx_proc.stdout_text
            log_to_file(f"RX Output:\n{rx_output}", rx_host, build)
            logger.info("RX output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving RX output: {e}")
            log_to_file(f"Error retrieving RX output: {e}", rx_host, build)

        try:
            log_to_file(f"TX Output:\n{tx_proc.stdout_text}", tx_host, build)
            logger.info("TX output captured successfully")
        except Exception as e:
            log_to_file(f"Error retrieving TX output: {e}", tx_host, build)

    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        if tx_proc:
            try:
                tx_proc.terminate()
            except Exception:
                pass
        if rx_proc:
            try:
                rx_proc.terminate()
            except Exception:
                pass
        raise
    finally:
        # Final cleanup - ensure processes are terminated
        if tx_proc:
            try:
                tx_proc.terminate()
                tx_proc.wait(timeout=3)
            except Exception:
                try:
                    tx_proc.kill()
                    tx_proc.wait(timeout=3)
                except Exception:
                    pass
        if rx_proc:
            try:
                rx_proc.terminate()
                rx_proc.wait(timeout=3)
            except Exception:
                try:
                    rx_proc.kill()
                    rx_proc.wait(timeout=3)
                except Exception:
                    pass
        if tcpdump:
            tcpdump.stop()

    if not check_output_rgb24(rx_output, 1):
        log_fail("rx video sessions failed")
        return False
    time.sleep(5)
    return True


def execute_dual_test_rgb24_multiple(
    test_time: int,
    build: str,
    tx_host,
    rx_host,
    type_: str,
    video_format_list: list,
    pg_format: str,
    video_url_list: list,
    capture_cfg=None,
):
    # Initialize logging for this test
    init_test_logging()

    # Use separate NIC port lists for TX and RX hosts
    tx_nic_port_list = tx_host.vfs
    rx_nic_port_list = rx_host.vfs
    video_size_1, fps_1 = decode_video_format_16_9(video_format_list[0])
    video_size_2, fps_2 = decode_video_format_16_9(video_format_list[1])

    logger.info(
        f"Creating RX config for RGB24 multiple dual test with video_formats: {video_format_list}"
    )
    log_to_file(
        f"Creating RX config for RGB24 multiple dual test with video_formats: {video_format_list}",
        rx_host,
        build,
    )
    try:
        rx_config_file = generate_rxtxapp_rx_config_multiple(
            rx_nic_port_list[:2], video_format_list, rx_host, build, True
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
        log_to_file(
            f"Successfully created RX config file: {rx_config_file}", rx_host, build
        )
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        log_to_file(f"Failed to create RX config file: {e}", rx_host, build)
        return False

    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_1_cmd = (
        f"ffmpeg -stream_loop -1 -video_size {video_size_1} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url_list[0]} -filter:v fps={fps_1} -p_port {tx_nic_port_list[0]} "
        f"-p_sip {ip_dict_rgb24_multiple['p_sip_1']} "
        f"-p_tx_ip {ip_dict_rgb24_multiple['p_tx_ip_1']} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )
    tx_2_cmd = (
        f"ffmpeg -stream_loop -1 -video_size {video_size_2} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url_list[1]} -filter:v fps={fps_2} -p_port {tx_nic_port_list[1]} "
        f"-p_sip {ip_dict_rgb24_multiple['p_sip_2']} "
        f"-p_tx_ip {ip_dict_rgb24_multiple['p_tx_ip_2']} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")
    logger.info(f"RX Command: {rx_cmd}")
    logger.info(f"TX1 Command: {tx_1_cmd}")
    logger.info(f"TX2 Command: {tx_2_cmd}")
    log_to_file(f"TX Host: {tx_host}", rx_host, build)
    log_to_file(f"RX Host: {rx_host}", rx_host, build)
    log_to_file(f"RX Command: {rx_cmd}", rx_host, build)
    log_to_file(f"TX1 Command: {tx_1_cmd}", tx_host, build)
    log_to_file(f"TX2 Command: {tx_2_cmd}", tx_host, build)

    rx_proc = None
    tx_1_proc = None
    tx_2_proc = None
    # Use RX host for tcpdump capture
    tcpdump = prepare_tcpdump(capture_cfg, rx_host)

    try:
        # Start RX pipeline first on RX host
        logger.info("Starting RX pipeline on RX host...")
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=rx_host,
            background=True,
            enable_sudo=True,
        )
        time.sleep(5)

        # Start TX pipelines on TX host
        logger.info("Starting TX pipelines on TX host...")
        tx_1_proc = run(
            tx_1_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=tx_host,
            background=True,
            enable_sudo=True,
        )
        tx_2_proc = run(
            tx_2_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=tx_host,
            background=True,
            enable_sudo=True,
        )

        # Start tcpdump after pipelines are running
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))

        logger.info(f"Waiting for RX process (test_time: {test_time} seconds)...")
        rx_proc.wait()
        logger.info("RX process completed")

        # Terminate TX processes after RX completes
        logger.info("Terminating TX processes...")
        for proc in [tx_1_proc, tx_2_proc]:
            if proc:
                try:
                    proc.terminate()
                    proc.wait(timeout=5)
                    logger.info("TX process terminated successfully")
                except Exception:
                    try:
                        proc.kill()
                        proc.wait(timeout=5)
                        logger.info("TX process killed")
                    except Exception:
                        logger.info("Could not terminate TX process")

        rx_output = ""
        try:
            rx_output = rx_proc.stdout_text
            log_to_file(f"RX Output:\n{rx_output}", rx_host, build)
            logger.info("RX output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving RX output: {e}")
            log_to_file(f"Error retrieving RX output: {e}", rx_host, build)

        try:
            log_to_file(f"TX1 Output:\n{tx_1_proc.stdout_text}", tx_host, build)
            logger.info("TX1 output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving TX1 output: {e}")

        try:
            log_to_file(f"TX2 Output:\n{tx_2_proc.stdout_text}", tx_host, build)
            logger.info("TX2 output captured successfully")
        except Exception as e:
            logger.info(f"Error retrieving TX2 output: {e}")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        for proc in [tx_1_proc, tx_2_proc, rx_proc]:
            if proc:
                try:
                    proc.terminate()
                except Exception:
                    pass
        raise
    finally:
        # Final cleanup - ensure processes are terminated
        for proc in [tx_1_proc, tx_2_proc, rx_proc]:
            if proc:
                try:
                    proc.terminate()
                    proc.wait(timeout=3)
                except Exception:
                    try:
                        proc.kill()
                        proc.wait(timeout=3)
                    except Exception:
                        pass
        if tcpdump:
            tcpdump.stop()

    if not check_output_rgb24(rx_output, 2):
        log_fail("rx video session failed")
        return False
    time.sleep(5)
    return True
