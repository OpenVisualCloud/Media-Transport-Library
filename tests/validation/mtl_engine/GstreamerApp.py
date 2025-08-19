# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import hashlib
import logging
import os
import time

from mtl_engine.RxTxApp import prepare_tcpdump

from .execute import log_fail, run, is_process_running, get_process_returncode

logger = logging.getLogger(__name__)


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

    return ":".join(plugin_paths)

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
        "filesrc",
        f"location={input_path}",
        "!",
        "rawaudioparse",
        "format=pcm",
        f"sample-rate={sampling}",
        f"pcm-format={audio_format}",
        f"num-channels={channels}",
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
    test_time: int = 10,
    host=None,
    sleep_interval: int = 10,
    tx_first: bool = True,
    capture_cfg=None,
):
    """
    Execute GStreamer test with remote host support following RxTxApp pattern.

    :param build: Build path on the remote host
    :param tx_command: TX pipeline command list
    :param rx_command: RX pipeline command list
    :param input_file: Input file path
    :param output_file: Output file path
    :param test_time: Test duration in seconds
    :param host: Remote host object with connection
    :param sleep_interval: Sleep interval between starting TX and RX
    :param tx_first: Whether to start TX first
    :return: True if test passed, False otherwise
    """
    case_id = os.environ.get("PYTEST_CURRENT_TEST", "gstreamer_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    remote_host = host

    logger.info(f"TX Command: {' '.join(tx_command)}")
    logger.info(f"RX Command: {' '.join(rx_command)}")

    tx_process = None
    rx_process = None
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
                host=remote_host,
                background=True,
                enable_sudo=True,
            )
            time.sleep(sleep_interval)

            # Start RX pipeline
            logger.info("Starting RX pipeline...")
            rx_process = run(
                " ".join(rx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=remote_host,
                background=True,
                enable_sudo=True,
            )
        else:
            # Start RX pipeline first
            logger.info("Starting RX pipeline...")
            rx_process = run(
                " ".join(rx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=remote_host,
                background=True,
                enable_sudo=True,
            )
            logger.info(f"Started RX process with PID: {getattr(rx_process, 'pid', None)}")

            # Start TX pipeline
            logger.info("Starting TX pipeline...")
            tx_process = run(
                " ".join(tx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=remote_host,
                background=True,
                enable_sudo=True,
            )
        # --- Start tcpdump after pipelines are running ---
        if tcpdump:
            logger.info("Starting tcpdump capture...")
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", test_time))

        # Let the test run for the specified duration
        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)

        if rx_process:
            try:
                logger.info(f"Terminating rx process (timeout == {sleep_interval})...")
                start_time = time.time()

                if hasattr(rx_process, 'wait'):
                    rx_process.wait(sleep_interval)
                else:
                    timeout_end = time.time() + sleep_interval
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
        if tcpdump:
            tcpdump.stop()

    # Compare files for validation
    file_compare = compare_files(input_file, output_file)
    logger.info(f"File comparison: {file_compare}")

    return file_compare


def build_gstreamer_command(pipeline_config: dict, build: str) -> list:
    """
    Build GStreamer command from pipeline configuration.

    :param pipeline_config: Pipeline configuration dictionary
    :param build: Build path
    :return: Command as list of strings
    """
    command = ["gst-launch-1.0", "-v"]

    # Add pipeline elements from config
    if "elements" in pipeline_config:
        command.extend(pipeline_config["elements"])

    # Add plugin path
    command.append(f"--gst-plugin-path={setup_gstreamer_plugins_paths(build)}")

    return command


def compare_files(input_file, output_file):
    if os.path.exists(input_file) and os.path.exists(output_file):
        input_file_size = os.path.getsize(input_file)
        output_file_size = os.path.getsize(output_file)
        logger.info(f"Input file size: {input_file_size}")
        logger.info(f"Output file size: {output_file_size}")
        if input_file_size != output_file_size:
            log_fail("File size is different")
            return False

        with open(input_file, "rb") as i_file, open(output_file, "rb") as o_file:
            i_hash = hashlib.md5(i_file.read()).hexdigest()
            o_hash = hashlib.md5(o_file.read()).hexdigest()
            logger.info(f"Input file hash: {i_hash}")
            logger.info(f"Output file hash: {o_hash}")
            if i_hash == o_hash:
                return True

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
