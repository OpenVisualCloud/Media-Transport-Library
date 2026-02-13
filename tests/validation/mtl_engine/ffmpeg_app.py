# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import copy
import json
import logging
import os
import re
import threading
import time

from mfd_connect import SSHConnection
from mfd_connect.exceptions import ConnectionCalledProcessError
from mtl_engine import ip_pools
from mtl_engine.const import FFMPEG_PATH, RXTXAPP_PATH

from . import rxtxapp_config
from .execute import log_fail, run

logger = logging.getLogger(__name__)

# Global variable to store timestamp for consistent logging
_log_timestamp = None


def capture_stdout(proc, proc_name: str):
    """Capture and log stdout from a process with timeout protection."""
    if proc is None:
        logger.debug(f"No process provided for {proc_name}")
        return ""

    output = ""

    def _get_output():
        nonlocal output
        try:
            output = proc.stdout_text
        except AttributeError:
            logger.debug(f"No stdout_text attribute for {proc_name}")
            output = ""
        except Exception as e:
            logger.warning(f"Error capturing stdout from {proc_name}: {e}")
            output = ""

    # Run in thread with timeout to avoid blocking
    capture_thread = threading.Thread(target=_get_output, daemon=True)
    capture_thread.start()
    capture_thread.join(timeout=5)

    if capture_thread.is_alive():
        logger.warning(f"Timeout capturing stdout from {proc_name}")
        return ""

    if output and output.strip():
        logger.info(f"{proc_name} Output:\n{output}")

    return output


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
    """Sanitize filename by replacing unsafe characters."""
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)


def get_process_pid(proc, proc_name: str, host) -> int:
    """Extract actual process PID using pgrep (not shell PID)."""
    if not host:
        return getattr(proc, "pid", None)

    search_term = "ffmpeg" if "ffmpeg" in proc_name.lower() else "RxTxApp"
    try:
        result = host.connection.execute_command(f"pgrep -n {search_term}")
        if result.return_code == 0 and result.stdout.strip():
            return int(result.stdout.strip())
    except (ValueError, AttributeError, ConnectionCalledProcessError):
        pass
    return getattr(proc, "pid", None)


def stop_process(proc, proc_name: str = "process", timeout: int = 5, host=None):
    """Stop process with hard timeout to prevent hanging.

    Uses threading to ensure we never hang forever. Forcefully kills if needed.

    Args:
        proc: Process object
        proc_name: Name for logging
        timeout: Maximum seconds before force kill
        host: Host connection object
    """
    if not proc:
        logger.debug(f"{proc_name}: No process to stop")
        return

    proc_pid = get_process_pid(proc, proc_name, host)
    logger.debug(f"{proc_name}: Stopping (PID: {proc_pid})")

    def _do_stop():
        # Step 1: Try graceful stop (max 2 seconds)
        try:
            if hasattr(proc, "running") and proc.running:
                logger.debug(f"{proc_name}: Attempting graceful stop")
                proc.stop(wait=2)
                if not proc.running:
                    logger.debug(f"{proc_name}: Stopped gracefully")
                    return
        except Exception as e:
            logger.debug(f"{proc_name}: Graceful stop failed: {e}")

        # Step 2: SIGTERM by PID
        if proc_pid and host:
            try:
                logger.debug(f"{proc_name}: Sending SIGTERM to PID {proc_pid}")
                host.connection.execute_command(
                    f"kill -15 {proc_pid} || true", shell=True
                )
                time.sleep(1)
            except Exception:
                pass

        # Step 3: Force SIGKILL
        try:
            if hasattr(proc, "kill"):
                proc.kill()
        except Exception:
            pass

        if proc_pid and host:
            try:
                logger.warning(f"{proc_name}: Force killing PID {proc_pid}")
                host.connection.execute_command(
                    f"kill -9 {proc_pid} || true", shell=True
                )
            except Exception:
                pass

    # Run with hard timeout
    stop_thread = threading.Thread(target=_do_stop, daemon=True)
    stop_thread.start()
    stop_thread.join(timeout=timeout)

    if stop_thread.is_alive():
        logger.error(f"{proc_name}: Stop timeout after {timeout}s, forcing SIGKILL")
        if proc_pid and host:
            try:
                host.connection.execute_command(
                    f"kill -9 {proc_pid} 2>/dev/null || true"
                )
            except Exception:
                pass

    logger.debug(f"{proc_name}: Stop completed")


def _kill_orphaned_processes_impl(host, process_pattern="ffmpeg", exclude_pids=None):
    """Kill orphaned processes matching pattern."""
    if not host:
        return

    exclude_pids = exclude_pids or []
    exclude_pids_set = {str(pid).strip() for pid in exclude_pids if pid}

    logger.debug(f"Checking for orphaned {process_pattern} processes...")
    try:
        # Find all processes matching the pattern
        result = host.connection.execute_command(f"pgrep -x '{process_pattern}'")
        if result.return_code == 0 and result.stdout.strip():
            all_pids = [
                pid.strip() for pid in result.stdout.strip().split("\n") if pid.strip()
            ]
            orphaned_pids = [pid for pid in all_pids if pid not in exclude_pids_set]

            if orphaned_pids:
                logger.warning(
                    f"Killing {len(orphaned_pids)} orphaned {process_pattern} process(es): {orphaned_pids}"
                )
                for pid in orphaned_pids:
                    try:
                        host.connection.execute_command(
                            f"kill -9 {pid} || true", shell=True
                        )
                    except Exception:
                        pass
            else:
                logger.debug(f"No orphaned {process_pattern} processes found")
    except Exception as e:
        logger.debug(f"Error in orphan cleanup: {e}")


def kill_orphaned_processes(host, process_pattern="ffmpeg", exclude_pids=None):
    """Kill orphaned processes with timeout protection (max 3 seconds)."""
    if not host:
        return

    def _cleanup():
        try:
            _kill_orphaned_processes_impl(host, process_pattern, exclude_pids)
        except Exception as e:
            logger.debug(f"Exception during orphan cleanup: {e}")

    cleanup_thread = threading.Thread(target=_cleanup, daemon=True)
    cleanup_thread.start()
    cleanup_thread.join(timeout=3)

    if cleanup_thread.is_alive():
        logger.warning(
            f"Orphan cleanup for {process_pattern} timeout after 3s, continuing"
        )


def execute_test(
    test_time: int,
    build: str,
    host,
    nic_port_list,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
    output_format: str,
    multiple_sessions: bool = False,
    tx_is_ffmpeg: bool = True,
):
    case_id = os.environ.get("PYTEST_CURRENT_TEST", "ffmpeg_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    video_size, fps = decode_video_format_16_9(video_format)
    match output_format:
        case "yuv":
            ffmpeg_rx_f_flag = "-f rawvideo"
        case "h264":
            ffmpeg_rx_f_flag = "-c:v libopenh264"
    if not multiple_sessions:
        output_files = create_empty_output_files(output_format, 1, host, build)
        rx_cmd = (
            f"{FFMPEG_PATH} -p_port {nic_port_list[0]} "
            f"-p_sip {ip_pools.rx[0]} "
            f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
            f"-payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i k "
            f"-init_retry 20 "
            f"{ffmpeg_rx_f_flag} {output_files[0]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"{FFMPEG_PATH} -video_size {video_size} -f rawvideo "
                f"-pix_fmt yuv422p10le -i {video_url} "
                f"-filter:v fps={fps} -p_port {nic_port_list[1]} "
                f"-p_sip {ip_pools.tx[0]} "
                f"-p_tx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
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
            f"{FFMPEG_PATH} -p_sip {ip_pools.rx[0]} "
            f"-p_port {nic_port_list[0]} "
            f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
            f"-payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 1 "
            f"-p_port {nic_port_list[0]} "
            f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20002 "
            f"-payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 2 "
            f"-map 0:0 {ffmpeg_rx_f_flag} {output_files[0]} -y "
            f"-map 1:0 {ffmpeg_rx_f_flag} {output_files[1]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"{FFMPEG_PATH} -video_size {video_size} -f rawvideo "
                f"-pix_fmt yuv422p10le -i {video_url} "
                f"-filter:v fps={fps} -p_port {nic_port_list[1]} "
                f"-p_sip {ip_pools.tx[0]} "
                f"-p_tx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 "
                f"-payload_type 112 -f mtl_st20p -"
            )
        else:  # tx is rxtxapp
            tx_config_file = generate_rxtxapp_tx_config(
                nic_port_list[1], video_format, video_url, host, build, True
            )
            tx_cmd = f"{RXTXAPP_PATH} --config_file {tx_config_file}"

    rx_proc = None
    tx_proc = None
    timeout = test_time + 90

    try:
        # Start RX pipeline first
        logger.info("Starting RX pipeline...")
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=timeout,
            testcmd=True,
            host=host,
            background=True,
        )
        time.sleep(5)  # Give RX time to initialize DPDK

        # Start TX pipeline
        logger.info("Starting TX pipeline...")
        tx_proc = run(
            tx_cmd,
            cwd=build,
            timeout=timeout,
            testcmd=True,
            host=host,
            background=True,
        )

        # Wait for test duration with proper timeout handling
        logger.info(f"Running test for {test_time} seconds with timeout {timeout}...")
        if tx_is_ffmpeg:
            # FFmpeg TX will complete after test_time, give it a 10s buffer
            tx_proc.wait(timeout=test_time + 10)
            logger.info("TX process completed")
        else:
            # RxTxApp runs indefinitely, just wait for test duration
            time.sleep(test_time)
            logger.info(f"Test duration {test_time}s completed")

    except Exception as e:
        logger.error(f"Error during test execution: {e}")
        raise
    finally:
        logger.info("Stopping processes...")

        # Stop TX first
        if tx_proc:
            stop_process(tx_proc, "TX", timeout=5, host=host)

        # Stop RX second
        if rx_proc:
            stop_process(rx_proc, "RX", timeout=5, host=host)

        # Wait for processes to fully terminate
        time.sleep(1)

        # Clean up any remaining orphaned processes
        kill_orphaned_processes(host, "ffmpeg")
        kill_orphaned_processes(host, "RxTxApp")

        # Capture output after processes stopped
        capture_stdout(rx_proc, "RX")
        capture_stdout(tx_proc, "TX")
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
    except Exception as e:
        logger.info(f"Could not remove output files: {e}")
    if not passed:
        log_fail("test failed")
    return passed


def execute_test_rgb24(
    test_time: int,
    build: str,
    host,
    nic_port_list,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
):
    # Initialize logging for this test
    init_test_logging()
    video_size, fps = decode_video_format_16_9(video_format)
    logger.info(f"Creating RX config for RGB24 test with video_format: {video_format}")
    try:
        rx_config_file = generate_rxtxapp_rx_config(
            nic_port_list[0], video_format, host, build
        )
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        return False
    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_cmd = (
        f"{FFMPEG_PATH} -stream_loop -1 -framerate {fps} -video_size {video_size} -f rawvideo -pix_fmt yuv422p10be "
        f"-i {video_url} -filter:v format=rgb24 -p_port {nic_port_list[1]} "
        f"-p_sip {ip_pools.tx[0]} -p_tx_ip {ip_pools.rx_multicast[0]} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    rx_proc = None
    tx_proc = None

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
        )

        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)
        logger.info(f"Test duration {test_time}s completed")

    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        raise
    finally:
        logger.info("Stopping processes...")

        # Stop TX (ffmpeg) first - this should stop gracefully
        if tx_proc:
            stop_process(tx_proc, "TX", timeout=5, host=host)

        # Stop RX (RxTxApp) second
        if rx_proc:
            stop_process(rx_proc, "RX", timeout=5, host=host)

        # Wait a moment for processes to fully terminate
        time.sleep(1)

        # Clean up any remaining orphaned processes
        kill_orphaned_processes(host, "ffmpeg")
        kill_orphaned_processes(host, "RxTxApp")

        # Capture output after processes stopped
        rx_output = capture_stdout(rx_proc, "RX")
        capture_stdout(tx_proc, "TX")
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
):
    """Execute RGB24 multiple streams test with comprehensive timeout protection."""

    # Maximum allowed runtime: test_time + setup/teardown buffer
    max_runtime = test_time + 120  # test time + 2 minute buffer
    start_time = time.time()

    def check_timeout():
        """Check if we've exceeded max runtime."""
        elapsed = time.time() - start_time
        if elapsed > max_runtime:
            logger.error(
                f"Test exceeded maximum runtime of {max_runtime}s (elapsed: {elapsed:.1f}s)"
            )
            return True
        return False

    video_size_1, fps_1 = decode_video_format_16_9(video_format_list[0])
    video_size_2, fps_2 = decode_video_format_16_9(video_format_list[1])
    logger.info(
        f"Creating RX config for RGB24 multiple test with video_formats: {video_format_list}"
    )

    if check_timeout():
        log_fail("Test timeout during initialization")
        return False

    try:
        rx_config_file = generate_rxtxapp_rx_config_multiple(
            nic_port_list[:2], video_format_list, host, build, True
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        return False
    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_1_cmd = (
        f"{FFMPEG_PATH} -stream_loop -1 -framerate {fps_1} -video_size {video_size_1} -f rawvideo -pix_fmt yuv422p10be "
        f"-i {video_url_list[0]} -filter:v format=rgb24 -p_port {nic_port_list[2]} "
        f"-p_sip {ip_pools.tx[0]} "
        f"-p_tx_ip {ip_pools.rx_multicast[0]} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )
    tx_2_cmd = (
        f"{FFMPEG_PATH} -stream_loop -1 -framerate {fps_2} -video_size {video_size_2} -f rawvideo -pix_fmt yuv422p10be "
        f"-i {video_url_list[1]} -filter:v format=rgb24 -p_port {nic_port_list[3]} "
        f"-p_sip {ip_pools.tx[1]} "
        f"-p_tx_ip {ip_pools.rx_multicast[1]} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    rx_proc = None
    tx_1_proc = None
    tx_2_proc = None

    if check_timeout():
        log_fail("Test timeout before starting processes")
        return False

    try:
        rx_proc = run(
            rx_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
        )
        time.sleep(5)

        if check_timeout():
            raise TimeoutError("Test timeout after starting RX")

        # Start TX pipelines
        logger.info("Starting TX pipelines...")
        tx_1_proc = run(
            tx_1_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
        )

        if check_timeout():
            raise TimeoutError("Test timeout after starting TX1")

        tx_2_proc = run(
            tx_2_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=host,
            background=True,
        )

        if check_timeout():
            raise TimeoutError("Test timeout after starting TX2")

        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)
        logger.info(f"Test duration {test_time}s completed")

    except TimeoutError as e:
        logger.error(f"Timeout occurred: {e}")
        log_fail(str(e))
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        raise
    finally:
        logger.info("Stopping processes...")

        # Stop TX processes (ffmpeg) first
        if tx_1_proc:
            stop_process(tx_1_proc, "TX1", timeout=5, host=host)
        if tx_2_proc:
            stop_process(tx_2_proc, "TX2", timeout=5, host=host)

        # Stop RX (RxTxApp) last
        if rx_proc:
            stop_process(rx_proc, "RX", timeout=5, host=host)

        # Wait for processes to fully terminate
        time.sleep(1)

        # Clean up any remaining orphaned processes
        kill_orphaned_processes(host, "ffmpeg")
        kill_orphaned_processes(host, "RxTxApp")

        # Capture output after processes stopped
        rx_output = capture_stdout(rx_proc, "RX")
        capture_stdout(tx_1_proc, "TX1")
        capture_stdout(tx_2_proc, "TX2")
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
        else:
            logger.info(f"Could not get input file size for {input_file}")
    except Exception as e:
        logger.info(f"Error checking input file size: {e}")

    # Use run() to check output file size
    stat_proc = run(f"stat -c '%s' {output_file}", host=host)

    if stat_proc.return_code == 0:
        output_file_size = int(stat_proc.stdout_text.strip())
        logger.info(f"Output file size: {output_file_size} bytes for {output_file}")
        result = output_file_size > 0
        logger.info(f"YUV check result: {result}")
        return result
    else:
        logger.info(f"Could not get output file size for {output_file}")
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
        else:
            logger.info(f"Could not get input file size for {input_file}")
    except Exception as e:
        logger.info(f"Error checking input file size: {e}")

    # Log output file size first
    try:
        stat_proc = run(f"stat -c '%s' {output_file}", host=host)
        if stat_proc.return_code == 0:
            output_file_size = int(stat_proc.stdout_text.strip())
            logger.info(f"Output file size: {output_file_size} bytes for {output_file}")
        else:
            logger.info(f"Could not get output file size for {output_file}")
    except Exception as e:
        logger.info(f"Error checking output file size: {e}")

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
        return result
    else:
        logger.info("H264 check failed")
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
        config["interfaces"][0]["ip"] = ip_pools.rx[0]
        config["rx_sessions"][0]["ip"][0] = ip_pools.rx_multicast[0]

        width, height, fps = decode_video_format_to_st20p(video_format)

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
        config["interfaces"][0]["ip"] = ip_pools.tx[0]
        config["rx_sessions"][0]["ip"][0] = ip_pools.rx_multicast[0]

        config["interfaces"][1]["name"] = nic_port_list[1]
        config["interfaces"][1]["ip"] = ip_pools.tx[1]
        config["rx_sessions"][1]["ip"][0] = ip_pools.rx_multicast[1]

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
        config["interfaces"][0]["ip"] = ip_pools.tx[0]
        config["tx_sessions"][0]["dip"][0] = ip_pools.rx_multicast[0]

        width, height, fps = decode_video_format_to_st20p(video_format)

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

        config_json = json.dumps(config, indent=4)
        remote_conn = host.connection
        f = remote_conn.path(config_file)
        if isinstance(remote_conn, SSHConnection):
            config_json = config_json.replace('"', '\\"')
        f.write_text(config_json, encoding="utf-8")

        return config_file

    except Exception as e:
        log_fail(f"Error generating TX ST20P config: {e}")
        raise


def decode_video_format_to_st20p(video_format: str) -> tuple:
    """Convert video format string to st20p parameters (width, height, fps)"""
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
            f"{FFMPEG_PATH} -p_port {rx_nic_port_list[0]} -p_sip {ip_pools.rx[0]} "
            f"-p_rx_ip {ip_pools.rx_multicast[0]} -udp_port 20000 -payload_type 112 "
            f"-fps {fps} -pix_fmt yuv422p10le -video_size {video_size} "
            f"-f mtl_st20p -i k {ffmpeg_rx_f_flag} {output_files[0]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"{FFMPEG_PATH} -video_size {video_size} -f rawvideo -pix_fmt yuv422p10le "
                f"-i {video_url} -filter:v fps={fps} -p_port {tx_nic_port_list[0]} "
                f"-p_sip {ip_pools.tx[0]} -p_tx_ip {ip_pools.rx_multicast[0]} "
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
            f"{FFMPEG_PATH} -p_sip {ip_pools.rx[0]} "
            f"-p_port {rx_nic_port_list[0]} -p_rx_ip {ip_pools.rx_multicast[0]} "
            f"-udp_port 20000 -payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 1 "
            f"-p_port {rx_nic_port_list[0]} -p_rx_ip {ip_pools.rx_multicast[0]} "
            f"-udp_port 20002 -payload_type 112 -fps {fps} -pix_fmt yuv422p10le "
            f"-video_size {video_size} -f mtl_st20p -i 2 "
            f"-map 0:0 {ffmpeg_rx_f_flag} {output_files[0]} -y "
            f"-map 1:0 {ffmpeg_rx_f_flag} {output_files[1]} -y"
        )
        if tx_is_ffmpeg:
            tx_cmd = (
                f"{FFMPEG_PATH} -video_size {video_size} -f rawvideo -pix_fmt yuv422p10le "
                f"-i {video_url} -filter:v fps={fps} -p_port {tx_nic_port_list[0]} "
                f"-p_sip {ip_pools.tx[0]} -p_tx_ip {ip_pools.rx_multicast[0]} "
                f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
            )
        else:  # tx is rxtxapp
            tx_config_file = generate_rxtxapp_tx_config(
                tx_nic_port_list[0], video_format, video_url, tx_host, build, True
            )
            tx_cmd = f"{RXTXAPP_PATH} --config_file {tx_config_file}"

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")

    rx_proc = None
    tx_proc = None

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
        )

        # Let the test run for the specified duration
        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)

        logger.info("Terminating processes...")
        if tx_proc:
            try:
                tx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        if rx_proc:
            try:
                rx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        # Wait a bit for termination
        time.sleep(2)
        # Get output after processes have been terminated
        capture_stdout(rx_proc, "RX")
        capture_stdout(tx_proc, "TX")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        if tx_proc:
            try:
                tx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        if rx_proc:
            try:
                rx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        raise
    finally:
        # Ensure processes are terminated
        if tx_proc:
            try:
                tx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        if rx_proc:
            try:
                rx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
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
    try:
        rx_config_file = generate_rxtxapp_rx_config(
            rx_nic_port_list[0], video_format, rx_host, build
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        return False

    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_cmd = (
        f"{FFMPEG_PATH} -stream_loop -1 -video_size {video_size} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url} -filter:v fps={fps} -p_port {tx_nic_port_list[0]} "
        f"-p_sip {ip_pools.tx[0]} -p_tx_ip {ip_pools.rx_multicast[0]} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")

    rx_proc = None
    tx_proc = None

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
        )
        logger.info(
            f"Waiting for RX process to complete (test_time: {test_time} seconds)..."
        )
        rx_proc.wait()
        logger.info("RX process completed")

        # Terminate TX process after RX completes
        logger.info("Terminating TX process...")
        if tx_proc:
            try:
                tx_proc.kill()
                logger.info("TX process killed")
            except Exception:
                logger.info("Could not terminate TX process")

        rx_output = capture_stdout(rx_proc, "RX")
        capture_stdout(tx_proc, "TX")

    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        if tx_proc:
            try:
                tx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        if rx_proc:
            try:
                rx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        raise
    finally:
        # Final cleanup - ensure processes are terminated
        if tx_proc:
            try:
                tx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass
        if rx_proc:
            try:
                rx_proc.kill()
            except Exception:
                # Process might already be terminated - ignore kill errors
                pass

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
    try:
        rx_config_file = generate_rxtxapp_rx_config_multiple(
            rx_nic_port_list[:2], video_format_list, rx_host, build, True
        )
        logger.info(f"Successfully created RX config file: {rx_config_file}")
    except Exception as e:
        log_fail(f"Failed to create RX config file: {e}")
        return False

    rx_cmd = f"{RXTXAPP_PATH} --config_file {rx_config_file} --test_time {test_time}"
    tx_1_cmd = (
        f"{FFMPEG_PATH} -stream_loop -1 -video_size {video_size_1} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url_list[0]} -filter:v fps={fps_1} -p_port {tx_nic_port_list[0]} "
        f"-p_sip {ip_pools.tx[0]} "
        f"-p_tx_ip {ip_pools.rx_multicast[0]} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )
    tx_2_cmd = (
        f"{FFMPEG_PATH} -stream_loop -1 -video_size {video_size_2} -f rawvideo -pix_fmt rgb24 "
        f"-i {video_url_list[1]} -filter:v fps={fps_2} -p_port {tx_nic_port_list[1]} "
        f"-p_sip {ip_pools.tx[1]} "
        f"-p_tx_ip {ip_pools.rx_multicast[1]} "
        f"-udp_port 20000 -payload_type 112 -f mtl_st20p -"
    )

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")

    rx_proc = None
    tx_1_proc = None
    tx_2_proc = None

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
        )
        tx_2_proc = run(
            tx_2_cmd,
            cwd=build,
            timeout=test_time + 60,
            testcmd=True,
            host=tx_host,
            background=True,
        )

        logger.info(f"Waiting for RX process (test_time: {test_time} seconds)...")
        rx_proc.wait()
        logger.info("RX process completed")

        # Terminate TX processes after RX completes
        logger.info("Terminating TX processes...")
        for proc in [tx_1_proc, tx_2_proc]:
            if proc:
                try:
                    proc.kill()
                    logger.info("TX process killed")
                except Exception:
                    logger.info("Could not terminate TX process")

        rx_output = capture_stdout(rx_proc, "RX")
        capture_stdout(tx_1_proc, "TX1")
        capture_stdout(tx_2_proc, "TX2")
    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        # Terminate processes immediately on error
        for proc in [tx_1_proc, tx_2_proc, rx_proc]:
            if proc:
                try:
                    proc.kill()
                except Exception:
                    # Process might already be terminated - ignore kill errors
                    pass
        raise
    finally:
        # Final cleanup - ensure processes are terminated
        for proc in [tx_1_proc, tx_2_proc, rx_proc]:
            if proc:
                try:
                    proc.kill()
                except Exception:
                    # Process might already be terminated - ignore kill errors
                    pass
    if not check_output_rgb24(rx_output, 2):
        log_fail("rx video session failed")
        return False
    time.sleep(5)
    return True
