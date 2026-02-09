# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import copy
import json
import logging
import os
import re
import time
from typing import List, Optional, Tuple

from mfd_connect import SSHConnection
from mtl_engine import ip_pools

from . import rxtxapp_config
from .execute import log_fail, run

RXTXAPP_PATH = "./tests/tools/RxTxApp/build/RxTxApp"
logger = logging.getLogger(__name__)

PTP_SYNC_TIME = 50  # seconds to wait for PTP synchronization


def capture_stdout(proc, proc_name: str):
    """Capture and log stdout from a process"""
    try:
        if proc and hasattr(proc, "stdout_text"):
            output = proc.stdout_text
            if output and output.strip():
                logger.info(f"{proc_name} Output:\n{output}")
            return output
        else:
            logger.debug(f"No stdout available for {proc_name}")
            return ""
    except Exception as e:
        logger.warning(f"Failed to capture stdout for {proc_name}: {e}")
        return ""


def create_empty_config() -> dict:
    return copy.deepcopy(rxtxapp_config.config_empty)


def create_empty_performance_config() -> dict:
    return copy.deepcopy(rxtxapp_config.config_performance_empty)


def set_tx_no_chain(config: dict, no_chain: bool) -> dict:
    config["tx_no_chain"] = no_chain
    return config


def add_interfaces(config: dict, nic_port_list: list, test_mode: str) -> dict:
    config["interfaces"][0]["name"] = nic_port_list[0]
    config["interfaces"][1]["name"] = nic_port_list[1]

    is_kernel = [
        nic_port_list[0].startswith("kernel:"),
        nic_port_list[1].startswith("kernel:"),
    ]

    # Check if using loopback interface (kernel:lo)
    is_loopback = [
        nic_port_list[0] == "kernel:lo",
        nic_port_list[1] == "kernel:lo",
    ]

    if test_mode in ("unicast", "multicast"):
        # Assign IPs to interfaces - use _os_ip for kernel interfaces, ip for others
        config["interfaces"][0]["_os_ip" if is_kernel[0] else "ip"] = ip_pools.tx[0]
        config["interfaces"][1]["_os_ip" if is_kernel[1] else "ip"] = ip_pools.rx[0]

        # Set session IPs based on mode
        if test_mode == "unicast":
            config["tx_sessions"][0]["dip"][0] = ip_pools.rx[0]
            config["rx_sessions"][0]["ip"][0] = ip_pools.tx[0]
        else:  # multicast
            config["tx_sessions"][0]["dip"][0] = ip_pools.rx_multicast[0]
            config["rx_sessions"][0]["ip"][0] = ip_pools.rx_multicast[0]

    elif test_mode == "kernel":
        # For loopback interface (kernel:lo), use 127.0.0.x addresses
        # For other kernel interfaces, use IP pools
        if is_loopback[0] and is_loopback[1]:
            # Both interfaces are loopback - use same IP for socket binding
            # TX sends from 127.0.0.1 to 127.0.0.1, RX binds to 127.0.0.1
            config["interfaces"][0]["_os_ip"] = "127.0.0.1"
            config["interfaces"][1]["_os_ip"] = "127.0.0.1"
            config["tx_sessions"][0]["dip"][0] = "127.0.0.1"
            config["rx_sessions"][0]["ip"][0] = "127.0.0.1"
        else:
            # Regular kernel interfaces - use IP pools
            for idx in (0, 1):
                if is_kernel[idx]:
                    config["interfaces"][idx]["_os_ip"] = (
                        ip_pools.tx[0] if idx == 0 else ip_pools.rx[0]
                    )

            config["tx_sessions"][0]["dip"][0] = ip_pools.rx[0]
            config["rx_sessions"][0]["ip"][0] = ip_pools.tx[0]
    else:
        log_fail(f"wrong test_mode {test_mode}")

    return config


def add_perf_video_session_tx(
    config: dict,
    nic_port: str,
    ip: str,
    dip: str,
    video_format: str,
    pg_format: str,
    video_url: str,
) -> dict:
    config["interfaces"].append(
        {
            "name": nic_port,
            "ip": ip,
        },
    )
    interface_id = len(config["interfaces"]) - 1
    session = copy.deepcopy(rxtxapp_config.config_performance_tx_video_session)
    session["dip"].append(dip)
    session["interface"].append(interface_id)
    session["video"][0]["video_format"] = video_format
    session["video"][0]["pg_format"] = pg_format
    session["video"][0]["video_url"] = video_url
    config["tx_sessions"].append(session)
    return config


def add_perf_video_session_rx(
    config: dict, nic_port: str, ip: str, sip: str, video_format: str, pg_format: str
) -> dict:
    config["interfaces"].append(
        {
            "name": nic_port,
            "ip": ip,
        },
    )
    interface_id = len(config["interfaces"]) - 1
    session = copy.deepcopy(rxtxapp_config.config_performance_rx_video_session)
    session["ip"].append(sip)
    session["interface"].append(interface_id)
    session["video"][0]["video_format"] = video_format
    session["video"][0]["pg_format"] = pg_format
    config["rx_sessions"].append(session)
    return config


def add_perf_st20p_session_tx(
    config: dict,
    nic_port: str,
    ip: str,
    dip: str,
    width: int,
    height: int,
    fps: str,
    input_format: str,
    transport_format: str,
    st20p_url: str,
) -> dict:
    config["interfaces"].append(
        {
            "name": nic_port,
            "ip": ip,
        },
    )
    interface_id = len(config["interfaces"]) - 1
    session = copy.deepcopy(rxtxapp_config.config_performance_tx_st20p_session)
    session["dip"].append(dip)
    session["interface"].append(interface_id)
    session["st20p"][0]["width"] = width
    session["st20p"][0]["height"] = height
    session["st20p"][0]["fps"] = fps
    session["st20p"][0]["input_format"] = input_format
    session["st20p"][0]["transport_format"] = transport_format
    session["st20p"][0]["st20p_url"] = st20p_url
    config["tx_sessions"].append(session)
    return config


def add_perf_st20p_session_rx(
    config: dict,
    nic_port: str,
    ip: str,
    sip: str,
    width: int,
    height: int,
    fps: str,
    output_format: str,
    transport_format: str,
) -> dict:
    config["interfaces"].append(
        {
            "name": nic_port,
            "ip": ip,
        },
    )
    interface_id = len(config["interfaces"]) - 1
    session = copy.deepcopy(rxtxapp_config.config_performance_rx_st20p_session)
    session["ip"].append(sip)
    session["interface"].append(interface_id)
    session["st20p"][0]["width"] = width
    session["st20p"][0]["height"] = height
    session["st20p"][0]["fps"] = fps
    session["st20p"][0]["output_format"] = output_format
    session["st20p"][0]["transport_format"] = transport_format
    config["rx_sessions"].append(session)
    return config


def add_video_sessions(
    config: dict,
    nic_port_list: list,
    test_mode: str,
    type_: str,
    video_format: str,
    pg_format: str,
    video_url: str,
) -> dict:
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )

    tx_session = copy.deepcopy(rxtxapp_config.config_tx_video_session)
    rx_session = copy.deepcopy(rxtxapp_config.config_rx_video_session)

    config["tx_sessions"][0]["video"].append(tx_session)
    config["rx_sessions"][0]["video"].append(rx_session)

    config["tx_sessions"][0]["video"][0]["type"] = type_
    config["rx_sessions"][0]["video"][0]["type"] = type_
    config["tx_sessions"][0]["video"][0]["video_format"] = video_format
    config["rx_sessions"][0]["video"][0]["video_format"] = video_format
    config["tx_sessions"][0]["video"][0]["pg_format"] = pg_format
    config["rx_sessions"][0]["video"][0]["pg_format"] = pg_format
    config["tx_sessions"][0]["video"][0]["video_url"] = video_url

    return config


def add_st20p_sessions(
    config: dict,
    nic_port_list: list,
    test_mode: str,
    width: int,
    height: int,
    fps: str,
    input_format: str,
    transport_format: str,
    output_format: str,
    st20p_url: str,
    interlaced: bool = False,
    pacing: str = "gap",
    packing: str = "BPM",
    enable_rtcp: bool = False,
    measure_latency: bool = False,
    out_url: str = "",
) -> dict:
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )
    tx_session = copy.deepcopy(rxtxapp_config.config_tx_st20p_session)
    config["tx_sessions"][0]["st20p"].append(tx_session)
    rx_session = copy.deepcopy(rxtxapp_config.config_rx_st20p_session)
    config["rx_sessions"][0]["st20p"].append(rx_session)

    config["tx_sessions"][0]["st20p"][0]["width"] = width
    config["rx_sessions"][0]["st20p"][0]["width"] = width
    config["tx_sessions"][0]["st20p"][0]["height"] = height
    config["rx_sessions"][0]["st20p"][0]["height"] = height
    config["tx_sessions"][0]["st20p"][0]["fps"] = fps
    config["rx_sessions"][0]["st20p"][0]["fps"] = fps
    config["tx_sessions"][0]["st20p"][0]["input_format"] = input_format
    config["tx_sessions"][0]["st20p"][0]["transport_format"] = transport_format
    config["rx_sessions"][0]["st20p"][0]["transport_format"] = transport_format
    config["rx_sessions"][0]["st20p"][0]["output_format"] = output_format
    config["tx_sessions"][0]["st20p"][0]["interlaced"] = interlaced
    config["rx_sessions"][0]["st20p"][0]["interlaced"] = interlaced
    config["tx_sessions"][0]["st20p"][0]["pacing"] = pacing
    config["rx_sessions"][0]["st20p"][0]["pacing"] = pacing
    config["tx_sessions"][0]["st20p"][0]["packing"] = packing
    config["rx_sessions"][0]["st20p"][0]["packing"] = packing
    config["tx_sessions"][0]["st20p"][0]["enable_rtcp"] = enable_rtcp
    config["rx_sessions"][0]["st20p"][0]["enable_rtcp"] = enable_rtcp
    config["rx_sessions"][0]["st20p"][0]["measure_latency"] = measure_latency
    config["tx_sessions"][0]["st20p"][0]["st20p_url"] = st20p_url
    config["rx_sessions"][0]["st20p"][0]["st20p_url"] = out_url

    return config


def add_st22p_sessions(
    config: dict,
    nic_port_list: list,
    test_mode: str,
    width: int,
    height: int,
    fps: str,
    codec: str,
    quality: str,
    input_format: str,
    output_format: str,
    st22p_url: str,
    codec_thread_count: str,
    interlaced: bool = False,
    measure_latency: bool = False,
) -> dict:
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )

    tx_session = copy.deepcopy(rxtxapp_config.config_tx_st22p_session)
    config["tx_sessions"][0]["st22p"].append(tx_session)

    config["tx_sessions"][0]["st22p"][0]["width"] = width
    config["tx_sessions"][0]["st22p"][0]["height"] = height
    config["tx_sessions"][0]["st22p"][0]["fps"] = fps
    config["tx_sessions"][0]["st22p"][0]["interlaced"] = interlaced
    config["tx_sessions"][0]["st22p"][0]["codec"] = codec
    config["tx_sessions"][0]["st22p"][0]["quality"] = quality
    config["tx_sessions"][0]["st22p"][0]["input_format"] = input_format
    config["tx_sessions"][0]["st22p"][0]["st22p_url"] = st22p_url
    config["tx_sessions"][0]["st22p"][0]["codec_thread_count"] = codec_thread_count

    rx_session = copy.deepcopy(rxtxapp_config.config_rx_st22p_session)
    config["rx_sessions"][0]["st22p"].append(rx_session)

    config["rx_sessions"][0]["st22p"][0]["width"] = width
    config["rx_sessions"][0]["st22p"][0]["height"] = height
    config["rx_sessions"][0]["st22p"][0]["fps"] = fps
    config["rx_sessions"][0]["st22p"][0]["interlaced"] = interlaced
    config["rx_sessions"][0]["st22p"][0]["codec"] = codec
    config["rx_sessions"][0]["st22p"][0]["quality"] = quality
    config["rx_sessions"][0]["st22p"][0]["output_format"] = output_format
    config["rx_sessions"][0]["st22p"][0]["codec_thread_count"] = codec_thread_count
    config["rx_sessions"][0]["st22p"][0]["measure_latency"] = measure_latency

    return config


def add_st30p_sessions(
    config: dict,
    nic_port_list: list,
    test_mode: str,
    filename: str,
    audio_format: str = "PCM24",
    audio_channel: list = ["U02"],
    audio_sampling: str = "96kHz",
    audio_ptime: str = "1",
    out_url: str = "",
) -> dict:
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )
    tx_session = copy.deepcopy(rxtxapp_config.config_tx_st30p_session)
    config["tx_sessions"][0]["st30p"].append(tx_session)
    rx_session = copy.deepcopy(rxtxapp_config.config_rx_st30p_session)
    config["rx_sessions"][0]["st30p"].append(rx_session)

    config["tx_sessions"][0]["st30p"][0]["audio_format"] = audio_format
    config["rx_sessions"][0]["st30p"][0]["audio_format"] = audio_format
    config["tx_sessions"][0]["st30p"][0]["audio_channel"] = audio_channel
    config["rx_sessions"][0]["st30p"][0]["audio_channel"] = audio_channel
    config["tx_sessions"][0]["st30p"][0]["audio_sampling"] = audio_sampling
    config["rx_sessions"][0]["st30p"][0]["audio_sampling"] = audio_sampling
    config["tx_sessions"][0]["st30p"][0]["audio_ptime"] = audio_ptime
    config["rx_sessions"][0]["st30p"][0]["audio_ptime"] = audio_ptime
    config["tx_sessions"][0]["st30p"][0]["audio_url"] = filename
    config["rx_sessions"][0]["st30p"][0]["audio_url"] = out_url

    return config


def add_audio_sessions(
    config: dict,
    nic_port_list: list,
    test_mode: str,
    type_: str,
    audio_format: str,
    audio_channel: list,
    audio_sampling: str,
    audio_ptime: str,
    audio_url: str,
) -> dict:
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )
    tx_session = copy.deepcopy(rxtxapp_config.config_tx_audio_session)
    config["tx_sessions"][0]["audio"].append(tx_session)
    rx_session = copy.deepcopy(rxtxapp_config.config_rx_audio_session)
    config["rx_sessions"][0]["audio"].append(rx_session)

    config["tx_sessions"][0]["audio"][0]["type"] = type_
    config["rx_sessions"][0]["audio"][0]["type"] = type_
    config["tx_sessions"][0]["audio"][0]["audio_format"] = audio_format
    config["rx_sessions"][0]["audio"][0]["audio_format"] = audio_format
    config["tx_sessions"][0]["audio"][0]["audio_channel"] = audio_channel
    config["rx_sessions"][0]["audio"][0]["audio_channel"] = audio_channel
    config["tx_sessions"][0]["audio"][0]["audio_sampling"] = audio_sampling
    config["rx_sessions"][0]["audio"][0]["audio_sampling"] = audio_sampling
    config["tx_sessions"][0]["audio"][0]["audio_ptime"] = audio_ptime
    config["rx_sessions"][0]["audio"][0]["audio_ptime"] = audio_ptime
    config["tx_sessions"][0]["audio"][0]["audio_url"] = audio_url
    config["rx_sessions"][0]["audio"][0]["audio_url"] = audio_url

    return config


def add_ancillary_sessions(
    config: dict,
    nic_port_list: list,
    test_mode: str,
    type_: str,
    ancillary_format: str,
    ancillary_fps: str,
    ancillary_url: str,
) -> dict:
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )
    tx_session = copy.deepcopy(rxtxapp_config.config_tx_ancillary_session)
    config["tx_sessions"][0]["ancillary"].append(tx_session)
    rx_session = copy.deepcopy(rxtxapp_config.config_rx_ancillary_session)
    config["rx_sessions"][0]["ancillary"].append(rx_session)

    config["tx_sessions"][0]["ancillary"][0]["type"] = type_
    config["tx_sessions"][0]["ancillary"][0]["ancillary_format"] = ancillary_format
    config["tx_sessions"][0]["ancillary"][0]["ancillary_fps"] = ancillary_fps
    config["tx_sessions"][0]["ancillary"][0]["ancillary_url"] = ancillary_url

    return config


def add_st41_sessions(
    config: dict,
    no_chain: bool,
    nic_port_list: list,
    test_mode: str,
    payload_type: str,
    type_: str,
    fastmetadata_data_item_type: str,
    fastmetadata_k_bit: str,
    fastmetadata_fps: str,
    fastmetadata_url: str,
) -> dict:
    config = set_tx_no_chain(config, no_chain)
    config = add_interfaces(
        config=config, nic_port_list=nic_port_list, test_mode=test_mode
    )
    tx_session = copy.deepcopy(rxtxapp_config.config_tx_st41_session)
    config["tx_sessions"][0]["fastmetadata"].append(tx_session)
    rx_session = copy.deepcopy(rxtxapp_config.config_rx_st41_session)
    config["rx_sessions"][0]["fastmetadata"].append(rx_session)

    config["tx_sessions"][0]["fastmetadata"][0]["payload_type"] = payload_type
    config["tx_sessions"][0]["fastmetadata"][0]["type"] = type_
    config["tx_sessions"][0]["fastmetadata"][0][
        "fastmetadata_data_item_type"
    ] = fastmetadata_data_item_type
    config["tx_sessions"][0]["fastmetadata"][0][
        "fastmetadata_k_bit"
    ] = fastmetadata_k_bit
    config["tx_sessions"][0]["fastmetadata"][0]["fastmetadata_fps"] = fastmetadata_fps
    config["tx_sessions"][0]["fastmetadata"][0]["fastmetadata_url"] = fastmetadata_url

    config["rx_sessions"][0]["fastmetadata"][0]["payload_type"] = payload_type
    config["rx_sessions"][0]["fastmetadata"][0][
        "fastmetadata_data_item_type"
    ] = fastmetadata_data_item_type
    config["rx_sessions"][0]["fastmetadata"][0][
        "fastmetadata_k_bit"
    ] = fastmetadata_k_bit
    config["rx_sessions"][0]["fastmetadata"][0]["fastmetadata_url"] = fastmetadata_url

    return config


# Global variable to store timestamp for consistent logging
_log_timestamp = None


def execute_test(
    config: dict,
    build: str,
    test_time: int,
    fail_on_error: bool = True,
    virtio_user: bool = False,
    rx_timing_parser: bool = False,
    ptp: bool = False,
    auto_stop: bool = False,
    rx_max_file_size: int = 0,
    host=None,
    netsniff=None,
    interface_setup=None,
) -> bool:

    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]

    config_json = json.dumps(config, indent=4)

    logger.info(f"Starting RxTxApp test: {get_case_id()}")

    remote_conn = host.connection

    # Configure kernel socket interfaces before creating config file
    # This must happen before MTL initialization
    configure_kernel_interfaces(config, remote_conn, interface_setup)

    config_file = f"{build}/tests/config.json"
    f = remote_conn.path(config_file)
    if isinstance(remote_conn, SSHConnection):
        config_json = config_json.replace('"', '\\"')
    f.write_text(config_json, encoding="utf-8")
    config_path = os.path.join(build, config_file)

    # Adjust test_time for high-res/fps/replicas
    if (
        "video" in config["tx_sessions"][0]
        and len(config["tx_sessions"][0]["video"]) > 0
    ):
        video_format = config["tx_sessions"][0]["video"][0]["video_format"]
        if any(format in video_format for format in ["4320", "2160"]):
            test_time = test_time * 2
            if any(fps in video_format for fps in ["p50", "p59", "p60", "p119"]):
                test_time = test_time * 2
            test_time = test_time * config["tx_sessions"][0]["video"][0]["replicas"]

    if (
        "st20p" in config["tx_sessions"][0]
        and len(config["tx_sessions"][0]["st20p"]) > 0
    ):
        video_format = config["tx_sessions"][0]["st20p"][0]["height"]
        video_fps = config["tx_sessions"][0]["st20p"][0]["fps"]
        if any(format == video_format for format in [4320, 2160]):
            test_time = test_time * 2
            if any(fps in video_fps for fps in ["p50", "p59", "p60", "p119"]):
                test_time = test_time * 2
            test_time = test_time * config["tx_sessions"][0]["st20p"][0]["replicas"]

    command = f"sudo {RXTXAPP_PATH} --config_file {config_path}"

    if virtio_user:
        command += " --virtio_user"

    if rx_timing_parser:
        command += " --rx_timing_parser"

    if ptp:
        command += " --ptp"
        test_time += PTP_SYNC_TIME  # Add extra time for PTP sync

    if auto_stop:
        command += " --auto_stop"

    if rx_max_file_size > 0:
        command += f" --rx_max_file_size {rx_max_file_size}"

    command += f" --test_time {test_time}"

    logger.info(f"RxTxApp Command: {command}")

    # For 4TX and 8k streams more timeout is needed
    timeout = test_time + 90
    if len(config["tx_sessions"]) >= 4 or any(
        format in str(config) for format in ["4320", "2160"]
    ):
        timeout = test_time + 120

    logger.info(f"Running RxTxApp for {test_time} seconds with timeout {timeout}")

    # Use run() for both local and remote
    cp = run(
        command, cwd=build, timeout=timeout, testcmd=True, host=host, background=True
    )

    if netsniff:
        if ptp:
            logger.info(
                f"Waiting {PTP_SYNC_TIME} seconds for PTP sync before netsniff-ng capture"
            )
            time.sleep(PTP_SYNC_TIME)
        netsniff.update_filter(dst_ip=config["tx_sessions"][0]["dip"][0])
        netsniff.capture(capture_time=test_time)
        logger.info(f"Finished netsniff-ng capture on host {host.name}")
    cp.wait(timeout=timeout)

    # Capture stdout output for logging
    logger.info(cp.stdout_text)

    output = cp.stdout_text.splitlines()

    # Check if process was killed or terminated unexpectedly
    bad_rc = {124: "timeout", 137: "SIGKILL", 143: "SIGTERM"}
    if cp.return_code != 0:
        if cp.return_code < 0:
            log_fail(f"RxTxApp was killed with signal {-cp.return_code}")
            return False
        for rc, reason in bad_rc.items():
            if cp.return_code == rc:
                log_fail(f"RxTxApp stopped by reason: {reason}")
                return False
        log_fail(f"RxTxApp returned non-zero exit code: {cp.return_code}")
        return False

    passed = True
    for session, check_output in zip(
        ("tx_sessions", "rx_sessions"), (check_tx_output, check_rx_output)
    ):
        if len(config[session][0]["video"]) > 0:
            passed = passed and check_output(
                config=config,
                output=output,
                session_type="video",
                fail_on_error=fail_on_error,
                host=host,
                build=build,
            )

    if len(config["tx_sessions"][0]["st20p"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="st20p",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )
        passed = passed and check_tx_converter_output(
            config=config,
            output=output,
            session_type="st20p",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )
        passed = passed and check_rx_converter_output(
            config=config,
            output=output,
            session_type="st20p",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )

    if len(config["tx_sessions"][0]["st22p"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="st22p",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )

    if len(config["tx_sessions"][0]["audio"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="audio",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )

    if len(config["tx_sessions"][0]["ancillary"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="anc",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )

    if len(config["tx_sessions"][0]["fastmetadata"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="fastmetadata",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )

    if len(config["tx_sessions"][0]["st30p"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="st30p",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )
    # Emit consolidated summary at end of log
    summary_lines, mismatch_found = _build_summary_block(
        config=config,
        output=output,
        command=command,
        test_time=test_time,
        timeout=timeout,
        passed=passed,
    )
    for line in summary_lines:
        logger.info(line)

    if mismatch_found:
        log_fail("Config/runtime mismatch detected; see summary for details")

    if passed:
        logger.info(f"RxTxApp test completed with result: {passed}")
    else:
        if fail_on_error:
            log_fail(f"RxTxApp test failed with result: {passed}")
        else:
            logger.info(f"RxTxApp test failed with result: {passed}")
    return passed


def execute_perf_test(
    config: dict,
    build: str,
    test_time: int,
    fail_on_error: bool = True,
    netsniff=None,
    host=None,
) -> bool:

    case_id = os.environ.get("PYTEST_CURRENT_TEST", "rxtxapp_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    config_json = json.dumps(config, indent=4)

    logger.info(f"Starting RxTxApp performance test: {get_case_id()}")
    logger.info(f"Performance test configuration: {config_json}")

    remote_conn = host.connection
    config_file = f"{build}/tests/config.json"
    f = remote_conn.path(config_file)
    if isinstance(remote_conn, SSHConnection):
        config_json = config_json.replace('"', '\\"')
    f.write_text(config_json, encoding="utf-8")
    config_path = os.path.join(build, config_file)

    command = f"sudo {RXTXAPP_PATH} --config_file {config_path} --test_time {test_time}"

    logger.info(f"Performance RxTxApp Command: {command}")

    # For 4TX and 8k streams more timeout is needed
    # Also scale timeout with replica count for performance tests
    total_replicas = 0
    for session in config.get("tx_sessions", []):
        for session_type in ["st20p", "video", "st22p", "audio", "st30p"]:
            if session_type in session and session[session_type]:
                for s in session[session_type]:
                    total_replicas += s.get("replicas", 1)

    # Base timeout
    timeout = test_time + 120

    # Additional timeout for high resolution content
    if any(format in str(config) for format in ["4320", "2160"]):
        timeout = test_time + 180

    # Scale timeout with replica count - more replicas need more time to stabilize
    if total_replicas > 1:
        # Add 10 seconds per replica beyond the first one, with a cap
        additional_timeout = min(
            total_replicas * 10, 300
        )  # Cap at 5 minutes additional
        timeout += additional_timeout
        logger.info(
            f"Scaling timeout for {total_replicas} replicas: +{additional_timeout}s"
        )

    logger.info(
        f"Running performance RxTxApp for {test_time} seconds with timeout {timeout}"
    )

    cp = run(
        command, cwd=build, timeout=timeout, testcmd=True, host=host, background=True
    )

    if netsniff:
        netsniff.update_filter(dst_ip=config["tx_sessions"][0]["dip"][0])
        netsniff.capture()
        logger.info(f"Finished netsniff-ng capture on host {netsniff.host.name}")

    cp.wait(timeout=timeout)

    # Capture stdout output for logging
    logger.info(cp.stdout_text)

    # Enhanced logging for process completion
    logger.info(f"Performance RxTxApp  ended with signal {cp.return_code}")

    # Check if process was killed or terminated unexpectedly
    if cp.return_code < 0:
        logger.info(f"Performance RxTxApp was killed with signal {cp.return_code}")
        return False
    elif cp.return_code == 124:  # timeout return code
        logger.info("Performance RxTxApp timed out")
        return False
    elif cp.return_code == 137:  # SIGKILL
        logger.info("Performance RxTxApp was killed (SIGKILL)")
        return False
    elif cp.return_code == 143:  # SIGTERM
        logger.info("Performance RxTxApp was terminated (SIGTERM)")
        return False

    # Get output lines
    output = cp.stdout_text.splitlines()

    if cp.return_code != 0:
        logger.info(
            f"Performance RxTxApp returned non-zero exit code: {cp.return_code}"
        )
        if cp.stderr_text:
            logger.info(f"Performance RxTxApp stderr: {cp.stderr_text}")
        # For performance tests, non-zero exit code means test failed
        return False

    # Determine session type based on config structure
    session_type = "video"  # default
    if len(config["tx_sessions"]) > 0:
        for session in config["tx_sessions"]:
            if "st20p" in session and len(session["st20p"]) > 0:
                session_type = "st20p"
                break
            elif "video" in session and len(session["video"]) > 0:
                session_type = "video"
                break

    logger.info(f"Performance test session type detected: {session_type}", host, build)

    result = check_tx_output(
        config=config,
        output=output,
        session_type=session_type,
        fail_on_error=fail_on_error,
        host=host,
        build=build,
    )

    # Emit consolidated summary for performance runs as well
    summary_lines, mismatch_found = _build_summary_block(
        config=config,
        output=output,
        command=command,
        test_time=test_time,
        timeout=timeout,
        passed=result,
    )
    for line in summary_lines:
        logger.info(line)

    if mismatch_found:
        log_fail("Config/runtime mismatch detected; see summary for details")

    logger.info(f"Performance RxTxApp test completed with result: {result}")

    return result


def save_as_json(json_file: str, content: dict):
    os.makedirs(os.path.dirname(json_file), exist_ok=True)
    with open(json_file, "w") as file_handle:
        json.dump(content, file_handle, indent=4)


def change_packing_video(content: dict, packing: str) -> dict:
    content["tx_sessions"][0]["video"][0]["packing"] = packing
    return content


def change_pacing_video(content: dict, pacing: str) -> dict:
    content["tx_sessions"][0]["video"][0]["pacing"] = pacing
    content["rx_sessions"][0]["video"][0]["pacing"] = pacing
    return content


def change_tr_offset_video(content: dict, tr_offset: str) -> dict:
    content["tx_sessions"][0]["video"][0]["tr_offset"] = tr_offset
    content["rx_sessions"][0]["video"][0]["tr_offset"] = tr_offset
    return content


def change_rss_mode(content: dict, rss_mode: str) -> dict:
    content["rss_mode"] = rss_mode
    return content


def change_replicas(
    config: dict, session_type: str, replicas: int, rx: bool = True
) -> dict:
    for i in range(len(config["tx_sessions"])):
        if config["tx_sessions"][i][session_type]:
            for j in range(len(config["tx_sessions"][i][session_type])):
                config["tx_sessions"][i][session_type][j]["replicas"] = replicas

    for i in range(len(config["rx_sessions"])):
        if config["rx_sessions"][i][session_type]:
            for j in range(len(config["rx_sessions"][i][session_type])):
                config["rx_sessions"][i][session_type][j]["replicas"] = replicas

    return config


def _count_ok_markers(output: List[str], pattern: re.Pattern[str]) -> int:
    """Count lines that match a pattern and contain the OK tag."""
    return sum(1 for line in output if pattern.search(line) and "OK" in line)


def _summarize_st20p(config: dict, output: List[str]) -> Tuple[List[str], bool]:
    """Build summary lines for st20p sessions and flag mismatches vs runtime logs."""
    lines: List[str] = []

    tx = config["tx_sessions"][0]["st20p"][0]
    rx = config["rx_sessions"][0]["st20p"][0]

    replicas_tx = tx.get("replicas", 1)
    replicas_rx = rx.get("replicas", 1)

    rx_ok = _count_ok_markers(output, re.compile(r"app_rx_st20p_result"))
    tx_conv_ok = 0
    rx_conv_ok = 0

    tx_conv_pattern = f"transport fmt ST20_FMT_{tx['transport_format'].upper()}, input fmt: {tx['input_format']}"
    rx_conv_pattern = f"transport fmt ST20_FMT_{rx['transport_format'].upper()}, output fmt {rx['output_format']}"

    # Capture actual formats seen in create logs (if present)
    tx_fmt_pattern = re.compile(
        r"mtl_video_tx_session_init\(\d+\), transport fmt ST20_FMT_([^,]+), input fmt: ([^, ]+)"
    )
    rx_fmt_pattern = re.compile(
        r"mtl_video_rx_session_init\(\d+\), transport fmt ST20_FMT_([^,]+), output fmt ([^, ]+)"
    )

    # Optional extraction for dimensions / interlaced / packing / pacing if present in logs
    tx_meta_pattern = re.compile(
        r"mtl_video_tx_session_init\(\d+\).*width (\d+), height (\d+).*interlaced (\d)"
    )
    rx_meta_pattern = re.compile(
        r"mtl_video_rx_session_init\(\d+\).*width (\d+), height (\d+).*interlaced (\d)"
    )
    tx_pack_pattern = re.compile(r"mtl_video_tx_session_init\(\d+\).*packing ([A-Z]+)")
    rx_pack_pattern = re.compile(r"mtl_video_rx_session_init\(\d+\).*packing ([A-Z]+)")
    tx_pace_pattern = re.compile(r"mtl_video_tx_session_init\(\d+\).*pacing ([A-Za-z]+)")
    rx_pace_pattern = re.compile(r"mtl_video_rx_session_init\(\d+\).*pacing ([A-Za-z]+)")

    actual_tx_transport = None
    actual_tx_input = None
    actual_rx_transport = None
    actual_rx_output = None
    actual_tx_width = None
    actual_tx_height = None
    actual_tx_interlaced = None
    actual_rx_width = None
    actual_rx_height = None
    actual_rx_interlaced = None
    actual_tx_packing = None
    actual_rx_packing = None
    actual_tx_pacing = None
    actual_rx_pacing = None

    for line in output:
        if tx_conv_pattern in line:
            tx_conv_ok += 1
        if rx_conv_pattern in line:
            rx_conv_ok += 1

        if actual_tx_transport is None:
            m = tx_fmt_pattern.search(line)
            if m:
                actual_tx_transport = m.group(1)
                actual_tx_input = m.group(2)

        if actual_rx_transport is None:
            m = rx_fmt_pattern.search(line)
            if m:
                actual_rx_transport = m.group(1)
                actual_rx_output = m.group(2)

        if actual_tx_width is None:
            m = tx_meta_pattern.search(line)
            if m:
                actual_tx_width = int(m.group(1))
                actual_tx_height = int(m.group(2))
                actual_tx_interlaced = m.group(3) == "1"

        if actual_rx_width is None:
            m = rx_meta_pattern.search(line)
            if m:
                actual_rx_width = int(m.group(1))
                actual_rx_height = int(m.group(2))
                actual_rx_interlaced = m.group(3) == "1"

        if actual_tx_packing is None:
            m = tx_pack_pattern.search(line)
            if m:
                actual_tx_packing = m.group(1)

        if actual_rx_packing is None:
            m = rx_pack_pattern.search(line)
            if m:
                actual_rx_packing = m.group(1)

        if actual_tx_pacing is None:
            m = tx_pace_pattern.search(line)
            if m:
                actual_tx_pacing = m.group(1)

        if actual_rx_pacing is None:
            m = rx_pace_pattern.search(line)
            if m:
                actual_rx_pacing = m.group(1)

    # FPS details per session
    fps_lines: List[str] = []
    expected_fps = _parse_expected_fps(rx.get("fps", ""))
    fps_results = _extract_rx_st20p_results(output)
    tolerance = expected_fps * 0.05 if expected_fps else None
    mismatches: List[str] = []
    for idx, status, measured_fps in fps_results:
        if expected_fps and tolerance is not None:
            delta = abs(measured_fps - expected_fps)
            fps_lines.append(
                (
                    f"idx={idx}:{status} fps={measured_fps:.2f} "
                    f"(expected~{expected_fps}, delta={delta:.2f}, tol={tolerance:.2f})"
                )
            )
            if delta > tolerance:
                mismatches.append(
                    (
                        f"fps session {idx} delta {delta:.2f} > tol {tolerance:.2f} "
                        f"(expected {expected_fps}, got {measured_fps:.2f})"
                    )
                )
        else:
            fps_lines.append(f"idx={idx}:{status} fps={measured_fps:.2f}")

    lines.append(
        "st20p config: "
        f"w={tx['width']} h={tx['height']} fps={tx['fps']} interlaced={tx['interlaced']} "
        f"input={tx['input_format']} transport={tx['transport_format']} output={rx['output_format']} "
        f"packing={tx.get('packing', '')} pacing={tx.get('pacing', '')} "
        f"replicas_tx={replicas_tx} replicas_rx={replicas_rx} "
        f"rtcp={tx.get('enable_rtcp', False)} latency={rx.get('measure_latency', False)}"
    )

    lines.append(
        "st20p results: "
        f"rx_ok={rx_ok}/{replicas_rx}; tx_conv_ok={tx_conv_ok}/{replicas_tx}; "
        f"rx_conv_ok={rx_conv_ok}/{replicas_rx}"
    )

    if fps_lines:
        lines.append("st20p fps: " + "; ".join(fps_lines))

    # Compare actual vs expected formats when present
    if actual_tx_transport and actual_tx_transport != tx["transport_format"].upper():
        mismatches.append(
            f"tx transport mismatch: expected {tx['transport_format'].upper()}, got {actual_tx_transport}"
        )
    if actual_tx_input and actual_tx_input != tx["input_format"]:
        mismatches.append(
            f"tx input fmt mismatch: expected {tx['input_format']}, got {actual_tx_input}"
        )
    if actual_rx_transport and actual_rx_transport != rx["transport_format"].upper():
        mismatches.append(
            f"rx transport mismatch: expected {rx['transport_format'].upper()}, got {actual_rx_transport}"
        )
    if actual_rx_output and actual_rx_output != rx["output_format"]:
        mismatches.append(
            f"rx output fmt mismatch: expected {rx['output_format']}, got {actual_rx_output}"
        )

    # Dimensions / interlaced / packing / pacing comparisons (only if observed in logs)
    if actual_tx_width is not None and actual_tx_width != tx["width"]:
        mismatches.append(
            f"tx width mismatch: expected {tx['width']}, got {actual_tx_width}"
        )
    if actual_tx_height is not None and actual_tx_height != tx["height"]:
        mismatches.append(
            f"tx height mismatch: expected {tx['height']}, got {actual_tx_height}"
        )
    if actual_tx_interlaced is not None and actual_tx_interlaced != tx["interlaced"]:
        mismatches.append(
            f"tx interlaced mismatch: expected {tx['interlaced']}, got {actual_tx_interlaced}"
        )
    if actual_tx_packing and actual_tx_packing != tx.get("packing", ""):
        mismatches.append(
            f"tx packing mismatch: expected {tx.get('packing', '')}, got {actual_tx_packing}"
        )
    if actual_tx_pacing and actual_tx_pacing != tx.get("pacing", ""):
        mismatches.append(
            f"tx pacing mismatch: expected {tx.get('pacing', '')}, got {actual_tx_pacing}"
        )

    if actual_rx_width is not None and actual_rx_width != rx["width"]:
        mismatches.append(
            f"rx width mismatch: expected {rx['width']}, got {actual_rx_width}"
        )
    if actual_rx_height is not None and actual_rx_height != rx["height"]:
        mismatches.append(
            f"rx height mismatch: expected {rx['height']}, got {actual_rx_height}"
        )
    if actual_rx_interlaced is not None and actual_rx_interlaced != rx["interlaced"]:
        mismatches.append(
            f"rx interlaced mismatch: expected {rx['interlaced']}, got {actual_rx_interlaced}"
        )
    if actual_rx_packing and actual_rx_packing != rx.get("packing", ""):
        mismatches.append(
            f"rx packing mismatch: expected {rx.get('packing', '')}, got {actual_rx_packing}"
        )
    if actual_rx_pacing and actual_rx_pacing != rx.get("pacing", ""):
        mismatches.append(
            f"rx pacing mismatch: expected {rx.get('pacing', '')}, got {actual_rx_pacing}"
        )

    missing_rx = replicas_rx - rx_ok
    missing_fps = replicas_rx - len(fps_results)
    if missing_rx > 0:
        mismatches.append(f"missing {missing_rx} RX OK marker(s)")
    if missing_fps > 0:
        mismatches.append(f"missing {missing_fps} fps result line(s)")
    if tx_conv_ok < replicas_tx:
        mismatches.append(
            f"missing TX converter lines: {tx_conv_ok}/{replicas_tx} found"
        )
    if rx_conv_ok < replicas_rx:
        mismatches.append(
            f"missing RX converter lines: {rx_conv_ok}/{replicas_rx} found"
        )

    lines.append(
        "st20p mismatches: " + ("; ".join(mismatches) if mismatches else "none")
    )

    return lines, bool(mismatches)


def _parse_expected_fps(fps: str) -> Optional[float]:
    """Extract numeric FPS from strings like ``p59`` or ``i50``."""
    match = re.match(r"[pi](\d+(?:\.\d+)?)", fps)
    if not match:
        logger.info(f"Unable to parse fps value from '{fps}'")
        return None
    try:
        return float(match.group(1))
    except ValueError:
        logger.info(f"FPS value is not numeric: '{fps}'")
        return None


def _extract_rx_st20p_results(output: List[str]) -> List[Tuple[int, str, float]]:
    """Parse st20p RX result lines for session index, status, and measured fps."""
    pattern = re.compile(
        r"app_rx_st20p_result\((\d+)\),\s+(OK|FAILED),\s+fps\s+([\d.]+)"
    )
    results: list[tuple[int, str, float]] = []
    for line in output:
        match = pattern.search(line)
        if not match:
            continue
        try:
            idx = int(match.group(1))
            status = match.group(2)
            fps = float(match.group(3))
            results.append((idx, status, fps))
        except (ValueError, IndexError):
            logger.debug(f"Failed to parse st20p result line: {line}")
    return results


def check_tx_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
    host=None,
    build: str = "",
) -> bool:
    # Check if this is a performance config with st20p sessions
    is_performance_st20p = False
    if (
        len(config["tx_sessions"]) > 0
        and session_type == "st20p"
        and any(
            "st20p" in session
            and len(session.get("st20p", [])) > 0
            and "video" not in session
            or len(session.get("video", [])) == 0
            for session in config["tx_sessions"]
        )
    ):
        is_performance_st20p = True

    if is_performance_st20p:
        return check_tx_fps_performance(
            config, output, session_type, fail_on_error, host, build
        )

    # Regular check for OK results
    ok_cnt = 0
    logger.info(f"Checking TX {session_type} output for OK results")

    for line in output:
        if f"app_tx_{session_type}_result" in line and "OK" in line:
            ok_cnt += 1
            logger.info(f"Found TX {session_type} OK result: {line}")

    replicas = 0
    for session in config["tx_sessions"]:
        if session[session_type]:
            for s in session[session_type]:
                replicas += s["replicas"]

    logger.info(f"TX {session_type} check: {ok_cnt}/{replicas} OK results found")

    if ok_cnt == replicas:
        logger.info(f"TX {session_type} check PASSED: all {replicas} sessions OK")
        return True

    reason = f"tx {session_type} session failed: {ok_cnt}/{replicas} OK markers found"
    if fail_on_error:
        log_fail(reason)
    else:
        logger.info(reason)

    return False


def check_tx_fps_performance(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
    host=None,
    build: str = "",
) -> bool:

    # Get expected FPS from config
    expected_fps = None
    replicas = 0

    for session in config["tx_sessions"]:
        if session_type in session and session[session_type]:
            for s in session[session_type]:
                fps_str = s.get("fps", "")
                if fps_str.startswith("p") or fps_str.startswith("i"):
                    expected_fps = int(fps_str[1:])  # Remove 'p' or 'i' prefix
                replicas += s["replicas"]
                break

    if expected_fps is None:
        logger.info("Could not determine expected FPS from config")
        return False

    logger.info(
        f"Checking TX FPS performance: expected {expected_fps} fps for {replicas} replicas"
    )

    # Look for TX_VIDEO_SESSION fps lines - check if any reading reaches target
    fps_pattern = re.compile(
        r"TX_VIDEO_SESSION\(\d+,(\d+):app_tx_st20p_(\d+)\):\s+fps\s+([\d.]+)"
    )

    # Set to track which sessions have achieved target FPS
    successful_sessions = set()
    fps_tolerance = 2  # Allow 2 fps tolerance (e.g., 49 is pass for 50 fps)

    # Check all FPS values to see if any session reaches target
    for line in output:
        match = fps_pattern.search(line)
        if match:
            session_id = int(match.group(2))  # Extract session ID from app_tx_st20p_X
            actual_fps = float(match.group(3))

            # Check if FPS is within tolerance
            if abs(actual_fps - expected_fps) <= fps_tolerance:
                if session_id not in successful_sessions:
                    successful_sessions.add(session_id)

    successful_count = len(successful_sessions)
    logger.info(
        f"TX FPS performance check: {successful_count}/{replicas} sessions achieved target"
    )

    if successful_count == replicas:
        return True

    if fail_on_error:
        log_fail(
            f"tx {session_type} fps performance failed: {successful_count}/{replicas} sessions"
        )
    else:
        logger.info(
            f"tx {session_type} fps performance failed: {successful_count}/{replicas} sessions"
        )

    return False


def check_rx_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
    host=None,
    build: str = "",
) -> bool:
    ok_cnt = 0
    logger.info(f"Checking RX {session_type} output for OK results")

    pattern = re.compile(r"app_rx_.*_result")
    if session_type == "anc":
        pattern = re.compile(r"app_rx_anc_result")
        session_type = "ancillary"
    elif session_type == "ancillary":
        pattern = re.compile(r"app_rx_anc_result")
    elif session_type == "st20p":
        pattern = re.compile(r"app_rx_st20p_result")
    elif session_type == "st22p":
        pattern = re.compile(r"app_rx_st22p_result")
    elif session_type == "st30p":
        pattern = re.compile(r"app_rx_st30p_result")
    elif session_type == "video":
        pattern = re.compile(r"app_rx_video_result")
    elif session_type == "audio":
        pattern = re.compile(r"app_rx_audio_result")
    else:
        pattern = re.compile(r"app_rx_.*_result")

    for line in output:
        if pattern.search(line) and "OK" in line:
            ok_cnt += 1
            logger.info(f"Found RX {session_type} OK result: {line}")

    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    logger.info(f"RX {session_type} check: {ok_cnt}/{replicas} OK results found")

    failure_reasons: List[str] = []

    if session_type == "st20p":
        # Parse detailed st20p result lines to capture per-session status and fps
        results = _extract_rx_st20p_results(output)
        if not results:
            failure_reasons.append("no app_rx_st20p_result lines captured in log")
        else:
            expected_fps = _parse_expected_fps(
                config["rx_sessions"][0][session_type][0]["fps"]
            )
            for idx, status, measured_fps in results:
                if status != "OK":
                    failure_reasons.append(
                        f"session {idx} reported {status} at {measured_fps:.2f} fps"
                    )
                if expected_fps:
                    delta = abs(measured_fps - expected_fps)
                    tolerance = expected_fps * 0.05
                    if delta > tolerance:
                        failure_reasons.append(
                            f"session {idx} fps mismatch: expected ~{expected_fps}, got {measured_fps:.2f}"
                        )

            missing = replicas - len(results)
            if missing > 0:
                failure_reasons.append(
                    f"missing {missing} st20p result line(s) (replicas={replicas})"
                )

    if ok_cnt == replicas and not failure_reasons:
        logger.info(f"RX {session_type} check PASSED: all {replicas} sessions OK")
        return True

    if not failure_reasons:
        # Try to surface failure lines to aid debugging
        failure_lines = [
            line
            for line in output
            if pattern.search(line) and any(tag in line for tag in ("FAIL", "ERR"))
        ]
        if failure_lines:
            failure_reasons.append(f"failure lines: {'; '.join(failure_lines[:3])}")

    reason = f"rx {session_type} session failed: {ok_cnt}/{replicas} OK. " + (
        "; ".join(failure_reasons) if failure_reasons else ""
    )

    if fail_on_error:
        log_fail(reason)
    else:
        logger.info(reason)

    return False


def check_tx_converter_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
    host=None,
    build: str = "",
) -> bool:
    ok_cnt = 0
    transport_format = config["tx_sessions"][0]["st20p"][0]["transport_format"]
    input_format = config["tx_sessions"][0]["st20p"][0]["input_format"]

    logger.info(f"Checking TX {session_type} converter output")

    for line in output:
        if (
            f"mtl_video_tx_session_init({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, input fmt: {input_format}"
            in line
        ):
            ok_cnt += 1
            logger.info(f"Found TX converter creation: {line}")

    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["tx_sessions"][0][session_type][0]["replicas"]

    logger.info(
        f"TX {session_type} converter check: {ok_cnt}/{replicas} converters created"
    )

    if ok_cnt == replicas:
        logger.info(f"TX {session_type} converter check PASSED")
        return True

    reason = (
        f"tx {session_type} converter missing expected line: "
        f"transport={transport_format}, input={input_format}; "
        f"found {ok_cnt}/{replicas}"
    )
    if fail_on_error:
        log_fail(reason)
    else:
        logger.info(reason)

    return False


def check_rx_converter_output(
    config: dict,
    output: list,
    session_type: str,
    fail_on_error: bool,
    host=None,
    build: str = "",
) -> bool:
    ok_cnt = 0

    transport_format = config["rx_sessions"][0]["st20p"][0]["transport_format"]
    output_format = config["rx_sessions"][0]["st20p"][0]["output_format"]

    logger.info(f"Checking RX {session_type} converter output")

    for line in output:
        if (
            f"mtl_video_rx_session_init({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, output fmt {output_format}"
            in line
        ):
            ok_cnt += 1
            logger.info(f"Found RX converter creation: {line}")

    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    logger.info(
        f"RX {session_type} converter check: {ok_cnt}/{replicas} converters created"
    )

    if ok_cnt == replicas:
        logger.info(f"RX {session_type} converter check PASSED")
        return True

    reason = (
        f"rx {session_type} converter missing expected line: "
        f"transport={transport_format}, output={output_format}; "
        f"found {ok_cnt}/{replicas}"
    )
    if fail_on_error:
        log_fail(reason)
    else:
        logger.info(reason)

    return False


def check_and_set_ip(interface_name: str, ip_address: str, connection):
    """Configure IP address on a kernel network interface."""
    result = connection.execute_command(f"ip addr show {interface_name}", shell=True)

    ip_without_mask = ip_address.split("/")[0]
    if ip_without_mask in result.stdout:
        logger.debug(f"IP {ip_address} already configured on {interface_name}")
        return

    connection.execute_command(
        f"sudo ip addr add {ip_address} dev {interface_name}", shell=True
    )
    connection.execute_command(f"sudo ip link set {interface_name} up", shell=True)
    logger.info(f"Configured IP {ip_address} on {interface_name}")


def configure_kernel_interfaces(config: dict, connection, interface_setup=None):
    """Configure OS-level IP addresses for kernel socket interfaces."""
    for interface in config.get("interfaces", []):
        if "_os_ip" not in interface:
            continue

        if not interface["name"].startswith("kernel:"):
            continue

        if_name = interface["name"].replace("kernel:", "")
        ip_addr = interface["_os_ip"]

        # Add default /24 netmask if not specified
        if "/" not in ip_addr:
            ip_addr = f"{ip_addr}/24"

        check_and_set_ip(if_name, ip_addr, connection)

        # Register for cleanup if interface_setup provided
        if interface_setup is not None:
            interface_setup.register_ip_cleanup(connection, if_name, ip_addr)


def _build_summary_block(
    *,
    config: dict,
    output: List[str],
    command: str,
    test_time: int,
    timeout: int,
    passed: bool,
) -> Tuple[List[str], bool]:
    """Create a human-friendly test summary to append at the end of logs."""
    lines: List[str] = []
    mismatch_found = False

    # Heading
    outcome = "PASS" if passed else "FAIL"
    lines.append(
        f"Summary: result={outcome}; test_time={test_time}s; timeout={timeout}s; command={command}"
    )

    # Core config snapshot (interfaces and IPs)
    try:
        tx_intf = config["interfaces"][0]["name"]
        rx_intf = (
            config["interfaces"][1]["name"] if len(config["interfaces"]) > 1 else ""
        )
        tx_ip = config["interfaces"][0].get("ip", "")
        rx_ip = (
            config["interfaces"][1].get("ip", "")
            if len(config["interfaces"]) > 1
            else ""
        )
        lines.append(f"Interfaces: tx={tx_intf}({tx_ip}) rx={rx_intf}({rx_ip})")
    except Exception:
        lines.append("Interfaces: unavailable")

    # Session summaries
    if config["tx_sessions"][0].get("st20p"):
        st20p_lines, st20p_mismatch = _summarize_st20p(config, output)
        lines.extend(st20p_lines)
        mismatch_found = mismatch_found or st20p_mismatch

    if config["tx_sessions"][0].get("ancillary"):
        anc = config["tx_sessions"][0]["ancillary"][0]
        replicas = anc.get("replicas", 1)
        rx_ok = _count_ok_markers(output, re.compile(r"app_rx_anc_result"))
        lines.append(
            "ancillary: "
            f"format={anc.get('ancillary_format', '')} fps={anc.get('ancillary_fps', '')} "
            f"replicas={replicas} rx_ok={rx_ok}/{replicas}"
        )

    if config["tx_sessions"][0].get("audio"):
        audio = config["tx_sessions"][0]["audio"][0]
        replicas = audio.get("replicas", 1)
        rx_ok = _count_ok_markers(output, re.compile(r"app_rx_audio_result"))
        lines.append(
            "audio: "
            f"fmt={audio.get('audio_format', '')} ch={audio.get('audio_channel', '')} "
            f"fs={audio.get('audio_sampling', '')} replicas={replicas} rx_ok={rx_ok}/{replicas}"
        )

    if config["tx_sessions"][0].get("st30p"):
        st30p = config["tx_sessions"][0]["st30p"][0]
        replicas = st30p.get("replicas", 1)
        rx_ok = _count_ok_markers(output, re.compile(r"app_rx_st30p_result"))
        lines.append(
            "st30p: "
            f"fmt={st30p.get('audio_format', '')} ch={st30p.get('audio_channel', '')} "
            f"fs={st30p.get('audio_sampling', '')} replicas={replicas} rx_ok={rx_ok}/{replicas}"
        )

    if config["tx_sessions"][0].get("fastmetadata"):
        fmd = config["tx_sessions"][0]["fastmetadata"][0]
        replicas = fmd.get("replicas", 1)
        rx_ok = _count_ok_markers(output, re.compile(r"app_rx_fastmetadata_result"))
        lines.append(
            "fastmetadata: "
            f"type={fmd.get('type', '')} fps={fmd.get('fastmetadata_fps', '')} "
            f"replicas={replicas} rx_ok={rx_ok}/{replicas}"
        )

    if config["tx_sessions"][0].get("st22p"):
        st22p = config["tx_sessions"][0]["st22p"][0]
        replicas = st22p.get("replicas", 1)
        rx_ok = _count_ok_markers(output, re.compile(r"app_rx_st22p_result"))
        lines.append(
            "st22p: "
            f"w={st22p.get('width')} h={st22p.get('height')} fps={st22p.get('fps')} "
            f"codec={st22p.get('codec', '')} replicas={replicas} rx_ok={rx_ok}/{replicas}"
        )

    if config["tx_sessions"][0].get("video"):
        video = config["tx_sessions"][0]["video"][0]
        replicas = video.get("replicas", 1)
        rx_ok = _count_ok_markers(output, re.compile(r"app_rx_video_result"))
        lines.append(
            "video: "
            f"fmt={video.get('video_format', '')} pg={video.get('pg_format', '')} "
            f"pacing={video.get('pacing', '')} replicas={replicas} rx_ok={rx_ok}/{replicas}"
        )

    return lines, mismatch_found


def get_case_id() -> str:
    case_id = os.environ.get("PYTEST_CURRENT_TEST", "rxtxapp_test")
    # Extract the test function name and parameters
    full_case = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id
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


def create_empty_dual_config() -> dict:
    return {
        "tx_config": copy.deepcopy(rxtxapp_config.config_empty_tx),
        "rx_config": copy.deepcopy(rxtxapp_config.config_empty_rx),
    }


def add_dual_interfaces(
    tx_config: dict,
    rx_config: dict,
    tx_nic_port_list: list,
    rx_nic_port_list: list,
    test_mode: str,
) -> tuple:
    tx_config["interfaces"][0]["name"] = tx_nic_port_list[0]
    rx_config["interfaces"][0]["name"] = rx_nic_port_list[0]

    is_kernel_tx = tx_nic_port_list[0].startswith("kernel:")
    is_kernel_rx = rx_nic_port_list[0].startswith("kernel:")

    # Check if using loopback interface (kernel:lo)
    is_loopback_tx = tx_nic_port_list[0] == "kernel:lo"
    is_loopback_rx = rx_nic_port_list[0] == "kernel:lo"

    if test_mode in ("unicast", "multicast"):
        # Assign IPs to both configs
        tx_config["interfaces"][0]["ip"] = ip_pools.tx[0]
        rx_config["interfaces"][0]["ip"] = ip_pools.rx[0]

        # Set session IPs based on mode
        if test_mode == "unicast":
            tx_config["tx_sessions"][0]["dip"][0] = ip_pools.rx[0]
            rx_config["rx_sessions"][0]["ip"][0] = ip_pools.tx[0]
        else:  # multicast
            tx_config["tx_sessions"][0]["dip"][0] = ip_pools.rx_multicast[0]
            rx_config["rx_sessions"][0]["ip"][0] = ip_pools.rx_multicast[0]

        # Handle kernel interfaces - move IP to _os_ip marker for OS configuration
        if is_kernel_tx:
            tx_config["interfaces"][0]["_os_ip"] = tx_config["interfaces"][0]["ip"]
            del tx_config["interfaces"][0]["ip"]
        if is_kernel_rx:
            rx_config["interfaces"][0]["_os_ip"] = rx_config["interfaces"][0]["ip"]
            del rx_config["interfaces"][0]["ip"]

    elif test_mode == "kernel":
        # For loopback interface (kernel:lo), use 127.0.0.x addresses
        # For other kernel interfaces, use IP pools
        if is_loopback_tx and is_loopback_rx:
            # Both interfaces are loopback - use same IP for socket binding
            # TX sends from 127.0.0.1 to 127.0.0.1, RX binds to 127.0.0.1
            tx_config["interfaces"][0]["_os_ip"] = "127.0.0.1"
            rx_config["interfaces"][0]["_os_ip"] = "127.0.0.1"
            tx_config["tx_sessions"][0]["dip"][0] = "127.0.0.1"
            rx_config["rx_sessions"][0]["ip"][0] = "127.0.0.1"
        else:
            # Regular kernel interfaces - use IP pools
            tx_config["interfaces"][0]["_os_ip"] = ip_pools.tx[0]
            rx_config["interfaces"][0]["_os_ip"] = ip_pools.rx[0]

            tx_config["tx_sessions"][0]["dip"][0] = ip_pools.rx[0]
            rx_config["rx_sessions"][0]["ip"][0] = ip_pools.tx[0]
    else:
        log_fail(f"wrong test_mode {test_mode}")

    return tx_config, rx_config


def add_st20p_dual_sessions(
    config: dict,
    tx_nic_port_list: list,
    rx_nic_port_list: list,
    test_mode: str,
    width: int,
    height: int,
    fps: str,
    input_format: str,
    transport_format: str,
    output_format: str,
    st20p_url: str,
    interlaced: bool = False,
    pacing: str = "gap",
    packing: str = "BPM",
    enable_rtcp: bool = False,
    measure_latency: bool = False,
    out_url: str = "",
) -> dict:
    tx_config = config["tx_config"]
    rx_config = config["rx_config"]

    tx_config, rx_config = add_dual_interfaces(
        tx_config=tx_config,
        rx_config=rx_config,
        tx_nic_port_list=tx_nic_port_list,
        rx_nic_port_list=rx_nic_port_list,
        test_mode=test_mode,
    )

    tx_session = copy.deepcopy(rxtxapp_config.config_tx_st20p_session)
    tx_config["tx_sessions"][0]["st20p"].append(tx_session)

    rx_session = copy.deepcopy(rxtxapp_config.config_rx_st20p_session)
    rx_config["rx_sessions"][0]["st20p"].append(rx_session)

    # Configure TX session
    tx_config["tx_sessions"][0]["st20p"][0]["width"] = width
    tx_config["tx_sessions"][0]["st20p"][0]["height"] = height
    tx_config["tx_sessions"][0]["st20p"][0]["fps"] = fps
    tx_config["tx_sessions"][0]["st20p"][0]["input_format"] = input_format
    tx_config["tx_sessions"][0]["st20p"][0]["transport_format"] = transport_format
    tx_config["tx_sessions"][0]["st20p"][0]["interlaced"] = interlaced
    tx_config["tx_sessions"][0]["st20p"][0]["pacing"] = pacing
    tx_config["tx_sessions"][0]["st20p"][0]["packing"] = packing
    tx_config["tx_sessions"][0]["st20p"][0]["enable_rtcp"] = enable_rtcp
    tx_config["tx_sessions"][0]["st20p"][0]["st20p_url"] = st20p_url

    # Configure RX session
    rx_config["rx_sessions"][0]["st20p"][0]["width"] = width
    rx_config["rx_sessions"][0]["st20p"][0]["height"] = height
    rx_config["rx_sessions"][0]["st20p"][0]["fps"] = fps
    rx_config["rx_sessions"][0]["st20p"][0]["transport_format"] = transport_format
    rx_config["rx_sessions"][0]["st20p"][0]["output_format"] = output_format
    rx_config["rx_sessions"][0]["st20p"][0]["interlaced"] = interlaced
    rx_config["rx_sessions"][0]["st20p"][0]["pacing"] = pacing
    rx_config["rx_sessions"][0]["st20p"][0]["packing"] = packing
    rx_config["rx_sessions"][0]["st20p"][0]["enable_rtcp"] = enable_rtcp
    rx_config["rx_sessions"][0]["st20p"][0]["measure_latency"] = measure_latency
    rx_config["rx_sessions"][0]["st20p"][0]["st20p_url"] = out_url

    return {"tx_config": tx_config, "rx_config": rx_config}


def add_st30p_dual_sessions(
    config: dict,
    tx_nic_port_list: list,
    rx_nic_port_list: list,
    test_mode: str,
    filename: str,
    audio_format: str = "PCM24",
    audio_channel: list = ["U02"],
    audio_sampling: str = "96kHz",
    audio_ptime: str = "1",
    out_url: str = "",
) -> dict:
    tx_config = config["tx_config"]
    rx_config = config["rx_config"]

    tx_config, rx_config = add_dual_interfaces(
        tx_config=tx_config,
        rx_config=rx_config,
        tx_nic_port_list=tx_nic_port_list,
        rx_nic_port_list=rx_nic_port_list,
        test_mode=test_mode,
    )

    tx_session = copy.deepcopy(rxtxapp_config.config_tx_st30p_session)
    tx_config["tx_sessions"][0]["st30p"].append(tx_session)

    rx_session = copy.deepcopy(rxtxapp_config.config_rx_st30p_session)
    rx_config["rx_sessions"][0]["st30p"].append(rx_session)

    # Configure TX session
    tx_config["tx_sessions"][0]["st30p"][0]["audio_format"] = audio_format
    tx_config["tx_sessions"][0]["st30p"][0]["audio_channel"] = audio_channel
    tx_config["tx_sessions"][0]["st30p"][0]["audio_sampling"] = audio_sampling
    tx_config["tx_sessions"][0]["st30p"][0]["audio_ptime"] = audio_ptime
    tx_config["tx_sessions"][0]["st30p"][0]["audio_url"] = filename

    # Configure RX session
    rx_config["rx_sessions"][0]["st30p"][0]["audio_format"] = audio_format
    rx_config["rx_sessions"][0]["st30p"][0]["audio_channel"] = audio_channel
    rx_config["rx_sessions"][0]["st30p"][0]["audio_sampling"] = audio_sampling
    rx_config["rx_sessions"][0]["st30p"][0]["audio_ptime"] = audio_ptime
    rx_config["rx_sessions"][0]["st30p"][0]["audio_url"] = out_url

    return {"tx_config": tx_config, "rx_config": rx_config}


def add_st40p_dual_sessions(
    config: dict,
    tx_nic_port_list: list,
    rx_nic_port_list: list,
    test_mode: str,
    type_: str,
    ancillary_format: str,
    ancillary_fps: str,
    ancillary_url: str,
) -> dict:
    tx_config = config["tx_config"]
    rx_config = config["rx_config"]

    tx_config, rx_config = add_dual_interfaces(
        tx_config=tx_config,
        rx_config=rx_config,
        tx_nic_port_list=tx_nic_port_list,
        rx_nic_port_list=rx_nic_port_list,
        test_mode=test_mode,
    )

    tx_session = copy.deepcopy(rxtxapp_config.config_tx_ancillary_session)
    tx_config["tx_sessions"][0]["ancillary"].append(tx_session)
    tx_config["tx_sessions"][0]["ancillary"][0]["type"] = type_
    tx_config["tx_sessions"][0]["ancillary"][0]["ancillary_format"] = ancillary_format
    tx_config["tx_sessions"][0]["ancillary"][0]["ancillary_fps"] = ancillary_fps
    tx_config["tx_sessions"][0]["ancillary"][0]["ancillary_url"] = ancillary_url

    rx_session = copy.deepcopy(rxtxapp_config.config_rx_ancillary_session)
    rx_config["rx_sessions"][0]["ancillary"].append(rx_session)
    rx_config["rx_sessions"][0]["ancillary"][0]["type"] = type_
    rx_config["rx_sessions"][0]["ancillary"][0]["ancillary_format"] = ancillary_format
    rx_config["rx_sessions"][0]["ancillary"][0]["ancillary_fps"] = ancillary_fps
    rx_config["rx_sessions"][0]["ancillary"][0]["ancillary_url"] = ancillary_url

    return {"tx_config": tx_config, "rx_config": rx_config}


def execute_dual_test(
    config: dict,
    build: str,
    test_time: int,
    tx_host,
    rx_host,
    fail_on_error: bool = True,
    virtio_user: bool = False,
    rx_timing_parser: bool = False,
    ptp: bool = False,
    interface_setup=None,
) -> bool:
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]

    tx_config = config["tx_config"]
    rx_config = config["rx_config"]

    tx_config_json = json.dumps(tx_config, indent=4)
    rx_config_json = json.dumps(rx_config, indent=4)

    # Log test start
    logger.info(f"Starting dual RxTxApp test: {get_case_id()}")

    # Configure kernel socket interfaces before creating config files
    # This must happen before MTL initialization
    configure_kernel_interfaces(tx_config, tx_host.connection, interface_setup)
    configure_kernel_interfaces(rx_config, rx_host.connection, interface_setup)

    # Prepare TX config
    tx_config_file = f"{build}/tests/tx_config.json"
    tx_f = tx_host.connection.path(build, "tests", "tx_config.json")
    tx_json_content = tx_config_json.replace('"', '\\"')
    tx_f.write_text(tx_json_content)

    # Prepare RX config
    rx_config_file = f"{build}/tests/rx_config.json"
    rx_f = rx_host.connection.path(build, "tests", "rx_config.json")
    rx_json_content = rx_config_json.replace('"', '\\"')
    rx_f.write_text(rx_json_content)

    # Adjust test_time for high-res/fps/replicas
    if (
        "st20p" in tx_config["tx_sessions"][0]
        and len(tx_config["tx_sessions"][0]["st20p"]) > 0
    ):
        video_format = tx_config["tx_sessions"][0]["st20p"][0]["height"]
        video_fps = tx_config["tx_sessions"][0]["st20p"][0]["fps"]
        if any(format == video_format for format in [4320, 2160]):
            test_time = test_time * 2
            if any(fps in video_fps for fps in ["p50", "p59", "p60", "p119"]):
                test_time = test_time * 2
            test_time = test_time * tx_config["tx_sessions"][0]["st20p"][0]["replicas"]

    # Prepare commands
    base_command = f"sudo {RXTXAPP_PATH} --test_time {test_time}"
    if virtio_user:
        base_command += " --virtio_user"
    if rx_timing_parser:
        base_command += " --rx_timing_parser"
    if ptp:
        base_command += " --ptp"

    tx_command = f"{base_command} --config_file {tx_config_file}"
    rx_command = f"{base_command} --config_file {rx_config_file}"

    logger.info(f"TX Command: {tx_command}")
    logger.info(f"RX Command: {rx_command}")

    # Start RX first
    rx_cp = run(
        rx_command,
        cwd=build,
        timeout=test_time + 90,
        testcmd=True,
        host=rx_host,
        background=True,
    )

    # Start TX
    tx_cp = run(
        tx_command,
        cwd=build,
        timeout=test_time + 90,
        testcmd=True,
        host=tx_host,
    )

    # Wait for both processes
    tx_cp.wait()
    rx_cp.wait()

    # Capture stdout output for logging
    capture_stdout(tx_cp, "TX RxTxApp")
    capture_stdout(rx_cp, "RX RxTxApp")

    # Get output from both hosts
    tx_output = tx_cp.stdout_text.splitlines()
    rx_output = rx_cp.stdout_text.splitlines()

    # Check results
    passed = True

    if len(tx_config["tx_sessions"][0]["st20p"]) > 0:
        passed = passed and check_tx_output(
            config=tx_config,
            output=tx_output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )
        passed = passed and check_tx_converter_output(
            config=tx_config,
            output=tx_output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )

    if len(rx_config["rx_sessions"][0]["st20p"]) > 0:
        passed = passed and check_rx_output(
            config=rx_config,
            output=rx_output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )
        passed = passed and check_rx_converter_output(
            config=rx_config,
            output=rx_output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )

    if len(rx_config["rx_sessions"][0]["st30p"]) > 0:
        passed = passed and check_rx_output(
            config=rx_config,
            output=rx_output,
            session_type="st30p",
            fail_on_error=fail_on_error,
        )

    logger.info(f"Dual RxTxApp test completed with result: {passed}")

    return passed
