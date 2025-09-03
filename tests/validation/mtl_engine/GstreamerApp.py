# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os
import re
import time

from .execute import log_fail, run

logger = logging.getLogger(__name__)


def capture_stdout(process, process_name):
    """
    Safely capture stdout from a process with proper error handling.
    """
    if not process:
        return None
    try:
        return process.stdout_text
    except Exception as e:
        logger.info(f"Error retrieving {process_name} output: {e}")
        return None


def create_connection_params(
    dev_port: str,
    payload_type: str,
    dev_ip: str = "192.168.96.3",
    ip: str = "192.168.96.2",
    udp_port: int = 20000,
    is_tx: bool = True,
) -> dict:
    params = {
        "dev-port": dev_port,
        "dev-ip": dev_ip,
        "ip": ip,
        "udp-port": udp_port,
        "payload-type": payload_type,
    }
    if is_tx:
        params.update(
            {
                "dev-ip": ip,
                "ip": dev_ip,
            }
        )
    else:
        params.update(
            {
                "dev-ip": dev_ip,
                "ip": ip,
            }
        )
    return params


def fract_format(framerate: str) -> str:
    """
    Convert framerate to proper fractional format for GStreamer.
    Handles specific framerates: 2398/100, 24, 25, 2997/100, 30, 50, 5994/100, 60, 100, 11988/100, 120
    """
    # If already in fractional format, return as-is
    if "/" in framerate:
        return framerate

    # Convert specific decimal framerates to fractional format
    framerate_map = {
        "23.98": "2398/100",
        "23.976": "2398/100",
        "29.97": "2997/100",
        "59.94": "5994/100",
        "119.88": "11988/100",
    }

    # Check if it's one of the special decimal framerates
    if framerate in framerate_map:
        return framerate_map[framerate]

    # For integer framerates, add /1
    try:
        int(framerate)  # Validate it's a number
        return f"{framerate}/1"
    except ValueError:
        # If it's not a valid number, return as-is (let GStreamer handle the error)
        return framerate


def setup_gstreamer_st20p_tx_pipeline(
    build: str,
    nic_port_list: str,
    input_path: str,
    width: int,
    height: int,
    framerate: str,
    format: str,
    tx_payload_type: int,
    tx_queues: int,
    tx_framebuff_num: int = None,
    tx_fps: int = None,
):
    connection_params = create_connection_params(
        dev_port=nic_port_list, payload_type=tx_payload_type, udp_port=20000, is_tx=True
    )

    framerate = fract_format(framerate)

    # st20 tx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0",
        "-v",
        "filesrc",
        f"location={input_path}",
        "!",
        f"rawvideoparse format={format} height={height} width={width} framerate={framerate}",
        "!",
        "mtl_st20p_tx",
        f"tx-queues={tx_queues}",
    ]

    if tx_framebuff_num is not None:
        pipeline_command.append(f"tx-framebuff-num={tx_framebuff_num}")

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    if tx_fps is not None:
        pipeline_command.append(f"tx-fps={tx_fps}")

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def setup_gstreamer_st20p_rx_pipeline(
    build: str,
    nic_port_list: str,
    output_path: str,
    width: int,
    height: int,
    framerate: str,
    format: str,
    rx_payload_type: int,
    rx_queues: int,
    rx_framebuff_num: int = None,
    rx_fps: int = None,
):
    connection_params = create_connection_params(
        dev_port=nic_port_list,
        payload_type=rx_payload_type,
        udp_port=20000,
        is_tx=False,
    )

    framerate = fract_format(framerate)

    # st20 rx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0",
        "-v",
        "mtl_st20p_rx",
        f"rx-queues={rx_queues}",
        f"rx-pixel-format={format}",
        f"rx-height={height}",
        f"rx-width={width}",
        f"rx-fps={framerate}",
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    if rx_framebuff_num is not None:
        pipeline_command.append(f"rx-framebuff-num={rx_framebuff_num}")

    pipeline_command.extend(["!", "filesink", f"location={output_path}"])

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def setup_gstreamer_st30_tx_pipeline(
    build: str,
    nic_port_list: str,
    input_path: str,
    tx_payload_type: int,
    tx_queues: int,
    audio_format: str,
    channels: int,
    sampling: int,
):
    connection_params = create_connection_params(
        dev_port=nic_port_list, payload_type=tx_payload_type, udp_port=30000, is_tx=True
    )

    # st30 tx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0",
        "audiotestsrc",
        "!",
        f"audio/x-raw,format={audio_format},rate={sampling},channels={channels}",
        "!",
        "mtl_st30p_tx",
        f"tx-queues={tx_queues}",
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def setup_gstreamer_st30_rx_pipeline(
    build: str,
    nic_port_list: str,
    output_path: str,
    rx_payload_type: int,
    rx_queues: int,
    rx_audio_format: str,
    rx_channels: int,
    rx_sampling: int,
):
    connection_params = create_connection_params(
        dev_port=nic_port_list,
        payload_type=rx_payload_type,
        udp_port=30000,
        is_tx=False,
    )

    # st30 rx GStreamer command line
    pipeline_command = ["gst-launch-1.0", "-v", "mtl_st30p_rx"]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    for x in [
        f"rx-queues={rx_queues}",
        f"rx-audio-format={rx_audio_format}",
        f"rx-channel={rx_channels}",
        f"rx-sampling={rx_sampling}",
        "!",
        "filesink",
        f"location={output_path}",
    ]:
        pipeline_command.append(x)

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def setup_gstreamer_st40p_tx_pipeline(
    build: str,
    nic_port_list: str,
    input_path: str,
    tx_payload_type: int,
    tx_queues: int,
    tx_framebuff_cnt: int,
    tx_fps: int,
    tx_did: int,
    tx_sdid: int,
):
    connection_params = create_connection_params(
        dev_port=nic_port_list, payload_type=tx_payload_type, udp_port=40000, is_tx=True
    )

    # st40 tx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0",
        "filesrc",
        f"location={input_path}",
        "!",
        "mtl_st40p_tx",
        f"tx-queues={tx_queues}",
        f"tx-framebuff-cnt={tx_framebuff_cnt}",
        f"tx-fps={tx_fps}",
        f"tx-did={tx_did}",
        f"tx-sdid={tx_sdid}",
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def setup_gstreamer_st40p_rx_pipeline(
    build: str,
    nic_port_list: str,
    output_path: str,
    rx_payload_type: int,
    rx_queues: int,
    timeout: int,
):
    connection_params = create_connection_params(
        dev_port=nic_port_list,
        payload_type=str(rx_payload_type),
        udp_port=40000,
        is_tx=False,
    )

    # st40 rx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0",
        "-v",
        "mtl_st40_rx",
        f"rx-queues={rx_queues}",
        f"timeout={timeout}",
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    pipeline_command.extend(["!", "filesink", f"location={output_path}"])

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def execute_test(
    build: str,
    tx_command: dict,
    rx_command: dict,
    input_file: str,
    output_file: str,
    test_time: int = 30,
    host=None,
    tx_host=None,
    rx_host=None,
    sleep_interval: int = 4,
    tx_first: bool = True,
):
    """
    Execute GStreamer test with remote host support following RxTxApp pattern.
    Supports both single host and dual host configurations.

    :param build: Build path on the remote host
    :param tx_command: TX pipeline command list
    :param rx_command: RX pipeline command list
    :param input_file: Input file path
    :param output_file: Output file path
    :param test_time: Test duration in seconds
    :param host: Remote host object (for single host tests)
    :param tx_host: TX host object (for dual host tests)
    :param rx_host: RX host object (for dual host tests)
    :param sleep_interval: Sleep interval between starting TX and RX
    :param tx_first: Whether to start TX first
    :return: True if test passed, False otherwise
    """
    is_dual = tx_host is not None and rx_host is not None

    if is_dual:
        logger.info("Executing dual host GStreamer test")
        tx_remote_host = tx_host
        rx_remote_host = rx_host
    else:
        logger.info("Executing single host GStreamer test")
        tx_remote_host = rx_remote_host = host

    case_id = os.environ.get("PYTEST_CURRENT_TEST", "gstreamer_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    logger.info(f"TX Command: {' '.join(tx_command)}")
    logger.info(f"RX Command: {' '.join(rx_command)}")

    tx_process = None
    rx_process = None

    try:
        if tx_first:
            # Start TX pipeline first
            logger.info("Starting TX pipeline...")
            tx_process = run(
                " ".join(tx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=tx_remote_host,
                background=True,
            )
            time.sleep(sleep_interval)

            # Start RX pipeline
            logger.info("Starting RX pipeline...")
            rx_process = run(
                " ".join(rx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=rx_remote_host,
                background=True,
            )
        else:
            # Start RX pipeline first
            logger.info("Starting RX pipeline...")
            rx_process = run(
                " ".join(rx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=rx_remote_host,
                background=True,
            )
            time.sleep(sleep_interval)

            # Start TX pipeline
            logger.info("Starting TX pipeline...")
            tx_process = run(
                " ".join(tx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=tx_remote_host,
                background=True,
            )

        logger.info(
            f"Waiting for RX process to complete (test_time: {test_time} seconds)..."
        )

        try:
            rx_process.wait(timeout=test_time + 30)  # Allow extra time for cleanup
            logger.info("RX process completed naturally")
        except Exception:
            logger.info("RX process did not complete in time, will clean up")
        if tx_process:
            try:
                tx_process.wait(timeout=10)  # Give TX time to finish
                logger.info("TX process completed naturally")
            except Exception:
                logger.info("TX process cleanup needed")
                tx_process.kill()

        # Get output after processes have completed
        output_rx = capture_stdout(rx_process, "RX")
        if output_rx:
            logger.info(f"RX Output: {output_rx}")

        output_tx = capture_stdout(tx_process, "TX")
        if output_tx:
            logger.info(f"TX Output: {output_tx}")

    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        raise
    finally:
        # Ensure processes are terminated
        if tx_process:
            try:
                tx_process.kill()
                tx_process.wait(timeout=10)
            except Exception:
                pass
        if rx_process:
            try:
                rx_process.kill()
                rx_process.wait(timeout=10)
            except Exception:
                pass

    # Compare files for validation
    file_compare = compare_files(
        input_file, output_file, tx_remote_host, rx_remote_host
    )

    logger.info(f"File comparison: {file_compare}")

    return file_compare


def compare_files(input_file, output_file, input_host=None, output_host=None):
    """
    Compare files on remote hosts.
    For single host: input_host and output_host should be the same
    For dual host: input_host and output_host are different hosts
    Always assumes remote operations.
    If input file doesn't exist, only validates output file exists and has content.
    """
    try:
        # Check if input file exists (for cases where input file might not be created)
        input_stat_proc = run(f"stat -c '%s' {input_file}", host=input_host)
        input_file_exists = input_stat_proc.return_code == 0

        if input_file_exists:
            input_output = capture_stdout(input_stat_proc, "input_stat")
            if input_output:
                input_file_size = int(input_output.strip())
                logger.info(f"Input file size: {input_file_size}")
            else:
                log_fail("Could not get input file size")
                return False
        else:
            logger.info(
                f"Input file {input_file} does not exist - skipping input validation"
            )

        # Check output file size (always remote)
        output_stat_proc = run(f"stat -c '%s' {output_file}", host=output_host)
        if output_stat_proc.return_code != 0:
            log_fail(f"Could not access output file {output_file}")
            return False
        output_output = capture_stdout(output_stat_proc, "output_stat")
        if output_output:
            output_file_size = int(output_output.strip())
            logger.info(f"Output file size: {output_file_size}")
        else:
            log_fail("Could not get output file size")
            return False

        # If input file doesn't exist, just validate output file has content
        if not input_file_exists:
            if output_file_size > 0:
                logger.info("Output file validation passed (input file not created)")
                return True
            else:
                log_fail("Output file is empty")
                return False

        # If input file exists, do full comparison
        if input_file_size != output_file_size:
            log_fail("File size is different")
            return False

        input_hash_proc = run(f"md5sum {input_file}", host=input_host)
        if input_hash_proc.return_code != 0:
            log_fail(f"Could not calculate hash for input file {input_file}")
            return False
        input_hash_output = capture_stdout(input_hash_proc, "input_hash")
        i_hash = (
            input_hash_output.split()[0]
            if input_hash_output and input_hash_output.strip()
            else ""
        )

        output_hash_proc = run(f"md5sum {output_file}", host=output_host)
        if output_hash_proc.return_code != 0:
            log_fail(f"Could not calculate hash for output file {output_file}")
            return False
        output_hash_output = capture_stdout(output_hash_proc, "output_hash")
        o_hash = (
            output_hash_output.split()[0]
            if output_hash_output and output_hash_output.strip()
            else ""
        )

        logger.info(f"Input file hash: {i_hash}")
        logger.info(f"Output file hash: {o_hash}")

        if i_hash and o_hash and i_hash == o_hash:
            return True

    except Exception as e:
        log_fail(f"Error during file comparison: {e}")
        return False

    log_fail("Comparison of files failed")
    return False


def video_format_change(file_format):
    if file_format in ["YUV422PLANAR10LE", "YUV_422_10bit"]:
        return "I422_10LE"
    else:
        return file_format


def audio_format_change(file_format, rx_side: bool = False):
    if rx_side:
        if file_format == "s8":
            return "PCM8"
        elif file_format == "s16le":
            return "PCM16"
        else:
            return "PCM24"
    else:
        if file_format == "s8":
            return 8
        elif file_format == "s16le":
            return 16
        else:
            return 24


def get_case_id() -> str:
    """Get test case ID from environment"""
    case_id = os.environ.get("PYTEST_CURRENT_TEST", "gstreamer_test")
    # Extract the test function name and parameters
    full_case = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id
    # Get the test name after the last ::
    test_name = full_case.split("::")[-1]
    return test_name


def sanitize_filename(name: str) -> str:
    """Replace unsafe characters with underscores"""
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)
