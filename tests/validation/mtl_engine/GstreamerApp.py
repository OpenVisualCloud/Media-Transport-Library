# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import hashlib
import logging
import os
import time
import re

from mtl_engine.RxTxApp import prepare_tcpdump

from .execute import log_fail, run

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
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    pipeline_command.extend(["!", "filesink", f"location={output_path}"])

    pipeline_command.append(
        f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/"
    )

    return pipeline_command


def create_gstreamer_test_config(
    test_type: str,
    tx_host=None,
    rx_host=None,
    **kwargs
) -> dict:
    """
    Create a standardized GStreamer test configuration following RxTxApp pattern.
    
    :param test_type: Type of test (st20p, st30, st40p)
    :param tx_host: TX host object
    :param rx_host: RX host object
    :param kwargs: Additional configuration parameters
    :return: Configuration dictionary
    """
    base_config = {
        "test_type": test_type,
        "tx_host": tx_host,
        "rx_host": rx_host,
        "tx_dev_ip": "192.168.96.3",
        "rx_dev_ip": "192.168.96.2", 
        "multicast_ip": "239.168.85.20",
        "test_time": kwargs.get("test_time", 30),
        "queues": kwargs.get("queues", 4),
        "payload_type": kwargs.get("payload_type", 112),
    }
    
    # Add test-type specific configuration
    if test_type == "st20p":
        base_config.update({
            "udp_port": 20000,
            "width": kwargs.get("width", 1920),
            "height": kwargs.get("height", 1080),
            "framerate": kwargs.get("framerate", "p25"),
            "format": kwargs.get("format", "YUV422PLANAR10LE"),
        })
    elif test_type == "st30":
        base_config.update({
            "udp_port": 30000,
            "audio_format": kwargs.get("audio_format", "s16le"),
            "channels": kwargs.get("channels", 2),
            "sampling": kwargs.get("sampling", 48000),
        })
    elif test_type == "st40p":
        base_config.update({
            "udp_port": 40000,
            "tx_framebuff_cnt": kwargs.get("tx_framebuff_cnt", 3),
            "tx_fps": kwargs.get("tx_fps", 25),
            "tx_did": kwargs.get("tx_did", 0x41),
            "tx_sdid": kwargs.get("tx_sdid", 0x01),
            "timeout": kwargs.get("timeout", 40000),
        })
    
    # Add any additional parameters from kwargs
    for key, value in kwargs.items():
        if key not in base_config:
            base_config[key] = value
    
    return base_config


def execute_gstreamer_test_from_config(
    config: dict,
    build: str,
    input_file: str,
    is_dual: bool = False
) -> bool:
    """
    Execute GStreamer test from configuration, following RxTxApp pattern.
    
    :param config: Test configuration dictionary
    :param build: Build path
    :param input_file: Input file path
    :param is_dual: Whether this is a dual-host test
    :return: True if test passed, False otherwise
    """
    test_type = config["test_type"]
    
    if is_dual:
        if test_type == "st20p":
            return execute_dual_st20p_test(
                build=build,
                tx_nic_port=config["tx_host"].vfs[0],
                rx_nic_port=config["rx_host"].vfs[0],
                input_path=input_file,
                width=config["width"],
                height=config["height"],
                framerate=config["framerate"],
                format=config["format"],
                payload_type=config["payload_type"],
                queues=config["queues"],
                test_time=config["test_time"],
                tx_host=config["tx_host"],
                rx_host=config["rx_host"],
                capture_cfg=config.get("capture_cfg"),
            )
        elif test_type == "st30":
            return execute_dual_st30_test(
                build=build,
                tx_nic_port=config["tx_host"].vfs[0],
                rx_nic_port=config["rx_host"].vfs[0],
                input_path=input_file,
                payload_type=config["payload_type"],
                queues=config["queues"],
                audio_format=config["audio_format"],
                channels=config["channels"],
                sampling=config["sampling"],
                test_time=config["test_time"],
                tx_host=config["tx_host"],
                rx_host=config["rx_host"],
                capture_cfg=config.get("capture_cfg"),
            )
        elif test_type == "st40p":
            return execute_dual_st40_test(
                build=build,
                tx_nic_port=config["tx_host"].vfs[0],
                rx_nic_port=config["rx_host"].vfs[0],
                input_path=input_file,
                payload_type=config["payload_type"],
                queues=config["queues"],
                tx_framebuff_cnt=config["tx_framebuff_cnt"],
                tx_fps=config["tx_fps"],
                tx_did=config["tx_did"],
                tx_sdid=config["tx_sdid"],
                timeout=config["timeout"],
                test_time=config["test_time"],
                tx_host=config["tx_host"],
                rx_host=config["rx_host"],
                capture_cfg=config.get("capture_cfg"),
            )
    else:
        # Single host test execution would go here
        # Currently not implemented but could be added for consistency
        raise NotImplementedError("Single host GStreamer tests not implemented in config pattern")
    
    return False


def execute_test(
    build: str,
    tx_command: dict,
    rx_command: dict,
    input_file: str,
    output_file: str,
    test_time: int = 30,
    host=None,
    sleep_interval: int = 5,
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
            time.sleep(sleep_interval)

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

        # Terminate processes gracefully
        logger.info("Terminating processes...")
        if tx_process:
            try:
                tx_process.terminate()
            except Exception:
                pass
        if rx_process:
            try:
                rx_process.terminate()
            except Exception:
                pass

        # Wait a bit for termination
        time.sleep(2)

        # Get output after processes have been terminated
        try:
            if rx_process and hasattr(rx_process, "stdout_text"):
                output_rx = rx_process.stdout_text.splitlines()
                for line in output_rx:
                    logger.info(f"RX Output: {line}")
        except Exception:
            logger.info("Could not retrieve RX output")

        try:
            if tx_process and hasattr(tx_process, "stdout_text"):
                output_tx = tx_process.stdout_text.splitlines()
                for line in output_tx:
                    logger.info(f"TX Output: {line}")
        except Exception:
            logger.info("Could not retrieve TX output")

    except Exception as e:
        log_fail(f"Error during test execution: {e}")
        raise
    finally:
        # Ensure processes are terminated
        if tx_process:
            try:
                tx_process.terminate()
                tx_process.wait(timeout=10)
            except Exception:
                pass
        if rx_process:
            try:
                rx_process.terminate()
                rx_process.wait(timeout=10)
            except Exception:
                pass
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
    command.append(f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/")

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


def create_dual_connection_params(
    tx_dev_port: str,
    rx_dev_port: str,
    payload_type: str,
    tx_dev_ip: str = "192.168.96.3",
    rx_dev_ip: str = "192.168.96.2",
    ip: str = "239.168.85.20",
    udp_port: int = 20000,
) -> tuple:
    """Create connection parameters for dual host test"""
    tx_params = {
        "dev-port": tx_dev_port,
        "dev-ip": tx_dev_ip,
        "ip": ip,
        "udp-port": udp_port,
        "payload-type": payload_type,
    }
    
    rx_params = {
        "dev-port": rx_dev_port,
        "dev-ip": rx_dev_ip,
        "ip": ip,
        "udp-port": udp_port,
        "payload-type": payload_type,
    }
    
    return tx_params, rx_params


def _update_pipeline_for_dual_host(
    command: list, 
    is_tx: bool,
    tx_dev_ip: str = "192.168.96.3",
    rx_dev_ip: str = "192.168.96.2",
    multicast_ip: str = "239.168.85.20"
) -> list:
    """
    Update a single-host pipeline command for dual-host networking.
    
    :param command: Original pipeline command list
    :param is_tx: Whether this is a TX pipeline
    :param tx_dev_ip: TX host device IP
    :param rx_dev_ip: RX host device IP  
    :param multicast_ip: Multicast IP for communication
    :return: Updated command list
    """
    updated_command = command.copy()
    
    # Find and replace IP configuration for dual host setup
    for i, arg in enumerate(updated_command):
        if arg.startswith("dev-ip="):
            if is_tx:
                updated_command[i] = f"dev-ip={tx_dev_ip}"
            else:
                updated_command[i] = f"dev-ip={rx_dev_ip}"
        elif arg.startswith("ip="):
            # For dual host, use multicast IP
            updated_command[i] = f"ip={multicast_ip}"
    
    return updated_command


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
    import re
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)


def log_to_file(message: str, host, build: str):
    """Log message to a file on the remote host"""
    import time
    
    test_name = sanitize_filename(get_case_id())
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    log_file = f"{build}/tests/{test_name}_{timestamp}_gstreamer.log"

    from .execute import run
    
    # Ensure parent directory exists
    parent_dir = os.path.dirname(log_file)
    run(f"mkdir -p {parent_dir}", host=host)

    # Append to file with timestamp
    log_timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    log_entry = f"[{log_timestamp}] {message}\n"

    try:
        remote_conn = host.connection
        f = remote_conn.path(log_file)
        
        if f.exists():
            current_content = f.read_text()
            f.write_text(current_content + log_entry, encoding="utf-8")
        else:
            f.write_text(log_entry, encoding="utf-8")
    except Exception as e:
        logger.warning(f"Could not write to log file {log_file}: {e}")


def create_empty_output_files(output_suffix: str, number_of_files: int = 1, host=None, build: str = "") -> list:
    """Create empty output files on remote host"""
    output_files = []
    
    # Create a timestamp for uniqueness
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    test_name = sanitize_filename(get_case_id())

    for i in range(number_of_files):
        output_file = f"{build}/tests/{test_name}_{timestamp}_out_{i}.{output_suffix}"
        output_files.append(output_file)

        try:
            remote_conn = host.connection
            f = remote_conn.path(output_file)
            f.touch()
        except Exception as e:
            logger.warning(f"Could not create output file {output_file}: {e}")

    return output_files


def execute_dual_test(
    build: str,
    tx_command: list,
    rx_command: list,
    input_file: str,
    output_file: str,
    test_time: int = 30,
    tx_host=None,
    rx_host=None,
    sleep_interval: int = 5,
    tx_first: bool = True,
    capture_cfg=None,
):
    """
    Execute GStreamer dual test with separate TX and RX hosts.

    :param build: Build path on the remote hosts
    :param tx_command: TX pipeline command list
    :param rx_command: RX pipeline command list
    :param input_file: Input file path on TX host
    :param output_file: Output file path on RX host
    :param test_time: Test duration in seconds
    :param tx_host: TX host object with connection
    :param rx_host: RX host object with connection
    :param sleep_interval: Sleep interval between starting TX and RX
    :param tx_first: Whether to start TX first
    :param capture_cfg: Capture configuration for tcpdump
    :return: True if test passed, False otherwise
    """
    case_id = os.environ.get("PYTEST_CURRENT_TEST", "gstreamer_dual_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    logger.info(f"TX Host: {tx_host}")
    logger.info(f"RX Host: {rx_host}")
    logger.info(f"TX Command: {' '.join(tx_command)}")
    logger.info(f"RX Command: {' '.join(rx_command)}")
    
    log_to_file(f"TX Host: {tx_host}", rx_host, build)
    log_to_file(f"RX Host: {rx_host}", rx_host, build)
    log_to_file(f"TX Command: {' '.join(tx_command)}", tx_host, build)
    log_to_file(f"RX Command: {' '.join(rx_command)}", rx_host, build)

    tx_process = None
    rx_process = None
    # Use RX host for tcpdump capture
    tcpdump = prepare_tcpdump(capture_cfg, rx_host)

    try:
        if tx_first:
            # Start TX pipeline first on TX host
            logger.info("Starting TX pipeline on TX host...")
            tx_process = run(
                " ".join(tx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=tx_host,
                background=True,
                enable_sudo=True,
            )
            time.sleep(sleep_interval)

            # Start RX pipeline on RX host
            logger.info("Starting RX pipeline on RX host...")
            rx_process = run(
                " ".join(rx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=rx_host,
                background=True,
                enable_sudo=True,
            )
        else:
            # Start RX pipeline first on RX host
            logger.info("Starting RX pipeline on RX host...")
            rx_process = run(
                " ".join(rx_command),
                cwd=build,
                timeout=test_time + 60,
                testcmd=True,
                host=rx_host,
                background=True,
                enable_sudo=True,
            )
            time.sleep(sleep_interval)

            # Start TX pipeline on TX host
            logger.info("Starting TX pipeline on TX host...")
            tx_process = run(
                " ".join(tx_command),
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

        # Let the test run for the specified duration
        logger.info(f"Running test for {test_time} seconds...")
        time.sleep(test_time)

        # Terminate processes gracefully
        logger.info("Terminating processes...")
        if tx_process:
            try:
                tx_process.terminate()
            except Exception:
                pass
        if rx_process:
            try:
                rx_process.terminate()
            except Exception:
                pass

        # Wait a bit for termination
        time.sleep(2)

        # Get output after processes have been terminated
        try:
            if rx_process and hasattr(rx_process, "stdout_text"):
                output_rx = rx_process.stdout_text.splitlines()
                for line in output_rx:
                    logger.info(f"RX Output: {line}")
                log_to_file(f"RX Output:\n{rx_process.stdout_text}", rx_host, build)
        except Exception:
            logger.info("Could not retrieve RX output")

        try:
            if tx_process and hasattr(tx_process, "stdout_text"):
                output_tx = tx_process.stdout_text.splitlines()
                for line in output_tx:
                    logger.info(f"TX Output: {line}")
                log_to_file(f"TX Output:\n{tx_process.stdout_text}", tx_host, build)
        except Exception:
            logger.info("Could not retrieve TX output")

    except Exception as e:
        log_fail(f"Error during dual test execution: {e}")
        raise
    finally:
        # Ensure processes are terminated
        if tx_process:
            try:
                tx_process.terminate()
                tx_process.wait(timeout=10)
            except Exception:
                pass
        if rx_process:
            try:
                rx_process.terminate()
                rx_process.wait(timeout=10)
            except Exception:
                pass
        if tcpdump:
            tcpdump.stop()

    # Compare files for validation
    file_compare = compare_dual_files(input_file, output_file, tx_host, rx_host)
    logger.info(f"File comparison: {file_compare}")

    return file_compare


def compare_dual_files(input_file: str, output_file: str, tx_host, rx_host) -> bool:
    """Compare input file on TX host with output file on RX host"""
    try:
        # Get input file hash from TX host
        from .execute import run
        
        input_md5_proc = run(f"md5sum {input_file}", host=tx_host)
        if input_md5_proc.return_code != 0:
            log_fail(f"Could not get MD5 of input file: {input_file}")
            return False
        input_hash = input_md5_proc.stdout_text.split()[0]
        
        # Get input file size from TX host
        input_stat_proc = run(f"stat -c '%s' {input_file}", host=tx_host)
        if input_stat_proc.return_code != 0:
            log_fail(f"Could not get size of input file: {input_file}")
            return False
        input_size = int(input_stat_proc.stdout_text.strip())

        # Get output file hash from RX host
        output_md5_proc = run(f"md5sum {output_file}", host=rx_host)
        if output_md5_proc.return_code != 0:
            log_fail(f"Could not get MD5 of output file: {output_file}")
            return False
        output_hash = output_md5_proc.stdout_text.split()[0]
        
        # Get output file size from RX host
        output_stat_proc = run(f"stat -c '%s' {output_file}", host=rx_host)
        if output_stat_proc.return_code != 0:
            log_fail(f"Could not get size of output file: {output_file}")
            return False
        output_size = int(output_stat_proc.stdout_text.strip())

        logger.info(f"Input file size: {input_size}")
        logger.info(f"Output file size: {output_size}")
        logger.info(f"Input file hash: {input_hash}")
        logger.info(f"Output file hash: {output_hash}")

        if input_size != output_size:
            log_fail("File sizes are different")
            return False

        if input_hash != output_hash:
            log_fail("File hashes are different")
            return False

        return True

    except Exception as e:
        log_fail(f"Error comparing files: {e}")
        return False


def execute_dual_st20p_test(
    build: str,
    tx_nic_port: str,
    rx_nic_port: str,
    input_path: str,
    width: int,
    height: int,
    framerate: str,
    format: str,
    payload_type: int,
    queues: int = 1,
    test_time: int = 30,
    tx_host=None,
    rx_host=None,
    tx_framebuff_num: int = None,
    rx_framebuff_num: int = None,
    tx_fps: int = None,
    rx_fps: int = None,
    capture_cfg=None,
):
    """
    Execute ST20P dual test with GStreamer TX and RX on separate hosts.
    """
    # Create output file on RX host
    output_files = create_empty_output_files("yuv", 1, rx_host, build)
    output_path = output_files[0]

    # Setup TX pipeline using single host function
    tx_command = setup_gstreamer_st20p_tx_pipeline(
        build=build,
        nic_port_list=tx_nic_port,
        input_path=input_path,
        width=width,
        height=height,
        framerate=framerate,
        format=format,
        tx_payload_type=payload_type,
        tx_queues=queues,
        tx_framebuff_num=tx_framebuff_num,
        tx_fps=tx_fps,
    )

    # Setup RX pipeline using single host function  
    rx_command = setup_gstreamer_st20p_rx_pipeline(
        build=build,
        nic_port_list=rx_nic_port,
        output_path=output_path,
        width=width,
        height=height,
        framerate=framerate,
        format=format,
        rx_payload_type=payload_type,
        rx_queues=queues,
        rx_framebuff_num=rx_framebuff_num,
        rx_fps=rx_fps,
    )

    # Update commands for dual host networking
    tx_command = _update_pipeline_for_dual_host(tx_command, is_tx=True)
    rx_command = _update_pipeline_for_dual_host(rx_command, is_tx=False)

    # Execute dual test
    result = execute_dual_test(
        build=build,
        tx_command=tx_command,
        rx_command=rx_command,
        input_file=input_path,
        output_file=output_path,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
        capture_cfg=capture_cfg,
    )

    # Clean up output files
    try:
        from .execute import run
        run(f"rm -f {output_path}", host=rx_host)
        logger.info(f"Removed output file: {output_path}")
    except Exception as e:
        logger.info(f"Could not remove output file: {e}")

    return result


def execute_dual_st30_test(
    build: str,
    tx_nic_port: str,
    rx_nic_port: str,
    input_path: str,
    payload_type: int,
    queues: int = 1,
    audio_format: str = "s16le",
    channels: int = 2,
    sampling: int = 48000,
    test_time: int = 30,
    tx_host=None,
    rx_host=None,
    capture_cfg=None,
):
    # Create output file on RX host
    output_files = create_empty_output_files("pcm", 1, rx_host, build)
    output_path = output_files[0]

    # Setup TX pipeline using single host function
    tx_command = setup_gstreamer_st30_tx_pipeline(
        build=build,
        nic_port_list=tx_nic_port,
        input_path=input_path,
        tx_payload_type=payload_type,
        tx_queues=queues,
        audio_format=audio_format,
        channels=channels,
        sampling=sampling,
    )

    # Setup RX pipeline using single host function
    rx_command = setup_gstreamer_st30_rx_pipeline(
        build=build,
        nic_port_list=rx_nic_port,
        output_path=output_path,
        rx_payload_type=payload_type,
        rx_queues=queues,
        rx_audio_format=audio_format_change(audio_format, rx_side=True),
        rx_channels=channels,
        rx_sampling=sampling,
    )

    # Update commands for dual host networking
    tx_command = _update_pipeline_for_dual_host(tx_command, is_tx=True)
    rx_command = _update_pipeline_for_dual_host(rx_command, is_tx=False)

    # Execute dual test
    result = execute_dual_test(
        build=build,
        tx_command=tx_command,
        rx_command=rx_command,
        input_file=input_path,
        output_file=output_path,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
        capture_cfg=capture_cfg,
    )

    # Clean up output files
    try:
        from .execute import run
        run(f"rm -f {output_path}", host=rx_host)
        logger.info(f"Removed output file: {output_path}")
    except Exception as e:
        logger.info(f"Could not remove output file: {e}")

    return result


def execute_dual_st40_test(
    build: str,
    tx_nic_port: str,
    rx_nic_port: str,
    input_path: str,
    payload_type: int,
    queues: int = 1,
    tx_framebuff_cnt: int = 3,
    tx_fps: int = 25,
    tx_did: int = 0x41,
    tx_sdid: int = 0x01,
    timeout: int = 40000,
    test_time: int = 30,
    tx_host=None,
    rx_host=None,
    capture_cfg=None,
):
    # Create output file on RX host
    output_files = create_empty_output_files("anc", 1, rx_host, build)
    output_path = output_files[0]

    # Setup TX pipeline using single host function
    tx_command = setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=tx_nic_port,
        input_path=input_path,
        tx_payload_type=payload_type,
        tx_queues=queues,
        tx_framebuff_cnt=tx_framebuff_cnt,
        tx_fps=tx_fps,
        tx_did=tx_did,
        tx_sdid=tx_sdid,
    )

    # Setup RX pipeline using single host function
    rx_command = setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=rx_nic_port,
        output_path=output_path,
        rx_payload_type=payload_type,
        rx_queues=queues,
        timeout=timeout,
    )

    # Update commands for dual host networking
    tx_command = _update_pipeline_for_dual_host(tx_command, is_tx=True)
    rx_command = _update_pipeline_for_dual_host(rx_command, is_tx=False)

    # Execute dual test
    result = execute_dual_test(
        build=build,
        tx_command=tx_command,
        rx_command=rx_command,
        input_file=input_path,
        output_file=output_path,
        test_time=test_time,
        tx_host=tx_host,
        rx_host=rx_host,
        capture_cfg=capture_cfg,
    )

    # Clean up output files
    try:
        from .execute import run
        run(f"rm -f {output_path}", host=rx_host)
        logger.info(f"Removed output file: {output_path}")
    except Exception as e:
        logger.info(f"Could not remove output file: {e}")

    return result
