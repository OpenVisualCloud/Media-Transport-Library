# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os
import re
import time

from mtl_engine.RxTxApp import prepare_tcpdump

from .execute import log_fail, run, is_process_running

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

def setup_gstreamer_plugins_paths (build):
    plugin_paths = [
        f"{build}/ecosystem/gstreamer_plugin/builddir",
        f"{build}/tests/tools/gstreamer_tools/builddir"
    ]
    logging.info(f"Setting up GStreamer plugin paths: {plugin_paths}")

    return ":".join(plugin_paths)

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
        "-v"]

    rawvideoparse_supported_formats = ["v210"]


    if format in rawvideoparse_supported_formats:
        pipeline_command.extend([
            "filesrc",
            f"location={input_path}",
            "!",
            f"rawvideoparse format={format} height={height} width={width} framerate={framerate}",
            "!"])
    elif format == "I422_10LE":
        pipeline_command.extend([
            "filesrc",
            f"location={input_path}",
            f"blocksize={width * height * 10}"])

    pipeline_command.extend([
        "mtl_st20p_tx",
        f"tx-queues={tx_queues}"
    ])

    if tx_framebuff_num is not None:
        pipeline_command.append(f"tx-framebuff-num={tx_framebuff_num}")

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    if tx_fps is not None:
        pipeline_command.append(f"tx-fps={tx_fps}")

    pipeline_command.append(
        f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}"
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
        f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}"
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
        f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}"
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
        f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}"
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
    tx_rfc8331: bool = False,
    tx_user_pacing: bool = False,
    tx_user_controlled_pacing: bool = False,
    tx_user_controlled_pacing_offset: int = 0
):
    connection_params = create_connection_params(
        dev_port=nic_port_list, payload_type=tx_payload_type, udp_port=40000, is_tx=True
    )

    pipeline_command = ["gst-launch-1.0"]

    if tx_rfc8331:
        pipeline_command.extend(
            ["ancgenerator",
             f"num-frames={tx_fps * 10}",
             f"fps={tx_fps}",
             "!"])
    else:
        pipeline_command.extend(["filesrc", f"location={input_path}", "!"])

    if tx_user_pacing:
        pipeline_command.extend(
            ["timeinserter", "!"]
        )

    pipeline_command.extend(
        ["mtl_st40p_tx",
         f"tx-queues={tx_queues}",
         f"tx-framebuff-cnt={tx_framebuff_cnt}",
         f"tx-fps={tx_fps}",
         f"tx-did={tx_did}",
         f"tx-sdid={tx_sdid}",
         f"parse-8331-meta={'true' if tx_rfc8331 else 'false'}",
         f"use-pts-for-pacing={'true' if tx_user_controlled_pacing else 'false'}",
         f"pts-pacing-offset={tx_user_controlled_pacing_offset}"]
    )

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    pipeline_command.append(
        f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}"
    )

    return pipeline_command

def setup_gstreamer_st40p_rx_pipeline(
    build: str,
    nic_port_list: str,
    output_path: str,
    rx_payload_type: int,
    rx_queues: int,
    timeout: int,
    capture_metadata: bool = False
):
    connection_params = create_connection_params(
        dev_port=nic_port_list,
        payload_type=rx_payload_type,
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
        f"include-metadata-in-buffer={'true' if capture_metadata else 'false'}"
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    pipeline_command.extend(["!", "filesink", f"location={output_path}"])

    pipeline_command.append(
        f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}"
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
    capture_cfg=None,
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
    :param capture_cfg: Capture configuration
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

    if is_dual:
        tx_tcpdump = prepare_tcpdump(capture_cfg, tx_host) if capture_cfg else None
        # rx_tcpdump = prepare_tcpdump(capture_cfg, rx_host) if capture_cfg else None  # Unused
        tcpdump = tx_tcpdump
    else:
        tcpdump = prepare_tcpdump(capture_cfg, host)

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
            logger.info(f"TX process started starting to sleep for {sleep_interval} seconds...")
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
            logger.info("RX process started...")
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
            logger.info(f"RX process started starting to sleep for {sleep_interval} seconds...")
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
            logger.info("TX process started...")
        # --- Start tcpdump after pipelines are running ---
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))

        # Let the test run for the specified duration
        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)

        terminate_wait = sleep_interval + 10

        if rx_process:
            try:
                logger.info(f"Terminating rx process (timeout == {terminate_wait})...")
                start_time = time.time()

                if hasattr(rx_process, 'wait'):
                    rx_process.wait(terminate_wait)
                else:
                    timeout_end = time.time() + terminate_wait
                    while time.time() < timeout_end and is_process_running(rx_process):
                        time.sleep(0.1)

                end_time = time.time()
                if not is_process_running(rx_process):
                    logger.info(f"RX process exited gracefully after {end_time - start_time:.2f} seconds.")
                else:
                    logger.info("RX process did not exit gracefully, killing it.")
                    if hasattr(rx_process, 'kill'):
                        rx_process.kill()
                    elif hasattr(rx_process, 'terminate'):
                        rx_process.terminate()
            except Exception as e:
                logger.warning(f"Error terminating RX process: {e}")

        if tx_process:
            try:
                logger.info(f"Terminating tx process (timeout == {sleep_interval})...")
                start_time = time.time()

                if hasattr(tx_process, 'wait'):
                    tx_process.wait(sleep_interval)
                else:
                    timeout_end = time.time() + sleep_interval
                    while time.time() < timeout_end and is_process_running(tx_process):
                        time.sleep(0.1)

                end_time = time.time()
                if not is_process_running(tx_process):
                    logger.info(f"TX process exited gracefully after {end_time - start_time:.2f} seconds.")
                else:
                    logger.info("TX process did not exit gracefully, killing it.")
                    if hasattr(tx_process, 'kill'):
                        tx_process.kill()
                    elif hasattr(tx_process, 'terminate'):
                        tx_process.terminate()

            except Exception as e:
                logger.warning(f"Error terminating TX process: {e}")

        if (tx_process and is_process_running(tx_process)) or (rx_process and is_process_running(rx_process)):
            logger.warning("Something went wrong waiting for RX/TX to terminate")
            time.sleep(sleep_interval)

        if tx_process and is_process_running(tx_process):
            logger.error("Something went wrong killing the TX process")
            tx_process.kill()

        if rx_process and is_process_running(rx_process):
            logger.error("Something went wrong killing the RX process")
            rx_process.kill()

        try:
            if tx_process and hasattr(tx_process, "stdout_text"):
                output_tx = tx_process.stdout_text.splitlines()
                for line in output_tx:
                    logger.info(f"TX Output: {line}")
        except Exception:
            logger.info("Could not retrieve TX output")

        try:
            if rx_process and hasattr(rx_process, "stdout_text"):
                output_rx = rx_process.stdout_text.splitlines()
                for line in output_rx:
                    logger.info(f"RX Output: {line}")
        except Exception:
            logger.info("Could not retrieve RX output")
            try:
                if rx_process and hasattr(rx_process, "stdout_text"):
                    output_rx = rx_process.stdout_text.splitlines()
                    for line in output_rx:
                        logger.info(f"RX Output: {line}")
            except Exception:
                logger.info(f"Could not retrieve RX output")

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
        if tcpdump:
            tcpdump.stop()

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
