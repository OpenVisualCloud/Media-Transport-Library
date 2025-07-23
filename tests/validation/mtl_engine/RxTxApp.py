# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import copy
import json
import os
import re
import subprocess
import sys
import time

from create_pcap_file.tcpdump import TcpDumpRecorder

from . import rxtxapp_config
from .execute import log_fail, log_info, run

RXTXAPP_PATH = "./tests/tools/RxTxApp/build/RxTxApp"

json_filename = os.path.join(sys.path[0], "ip_addresses.json")

unicast_ip_dict = dict(
    tx_interfaces="192.168.17.101",
    rx_interfaces="192.168.17.102",
    tx_sessions="192.168.17.102",
    rx_sessions="192.168.17.101",
)

multicast_ip_dict = dict(
    tx_interfaces="192.168.17.101",
    rx_interfaces="192.168.17.102",
    tx_sessions="239.168.48.9",
    rx_sessions="239.168.48.9",
)

kernel_ip_dict = dict(
    tx_interfaces="192.168.17.101",
    rx_interfaces="192.168.17.102",
    tx_sessions="127.0.0.1",
    rx_sessions="127.0.0.1",
)


def read_ip_addresses_from_json(filename: str):
    global unicast_ip_dict, multicast_ip_dict, kernel_ip_dict
    try:
        with open(filename, "r") as ips:
            ip_addresses = json.load(ips)
            try:
                unicast_ip_dict = ip_addresses["unicast_ip_dict"]
                multicast_ip_dict = ip_addresses["multicast_ip_dict"]
                kernel_ip_dict = ip_addresses["kernel_ip_dict"]
            except Exception:
                print(
                    f"File {filename} does not contain proper input, check "
                    "ip_addresses.json.example for a schema. It might have "
                    "been loaded partially. Where not specified, "
                    "default was set."
                )
    except Exception:
        print(
            f"File {filename} could not be loaded properly! "
            "Default values are set for all IPs."
        )


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

    if test_mode == "unicast":
        config["interfaces"][0]["ip"] = unicast_ip_dict["tx_interfaces"]
        config["interfaces"][1]["ip"] = unicast_ip_dict["rx_interfaces"]
        config["tx_sessions"][0]["dip"][0] = unicast_ip_dict["tx_sessions"]
        config["rx_sessions"][0]["ip"][0] = unicast_ip_dict["rx_sessions"]
    elif test_mode == "multicast":
        config["interfaces"][0]["ip"] = multicast_ip_dict["tx_interfaces"]
        config["interfaces"][1]["ip"] = multicast_ip_dict["rx_interfaces"]
        config["tx_sessions"][0]["dip"][0] = multicast_ip_dict["tx_sessions"]
        config["rx_sessions"][0]["ip"][0] = multicast_ip_dict["rx_sessions"]
    elif test_mode == "kernel":
        config["tx_sessions"][0]["dip"][0] = kernel_ip_dict["tx_sessions"]
        config["rx_sessions"][0]["ip"][0] = kernel_ip_dict["rx_sessions"]
    else:
        log_fail(f"wrong test_mode {test_mode}")

    return config


def prepare_tcpdump(capture_cfg, host=None):
    """
    Prepare and (optionally) start TcpDumpRecorder if capture_cfg is enabled.
    Returns the TcpDumpRecorder instance or None.
    """
    if capture_cfg and capture_cfg.get("enable"):
        tcpdump = TcpDumpRecorder(
            host=host,
            test_name=capture_cfg.get("test_name", "capture"),
            pcap_dir=capture_cfg.get("pcap_dir", "/tmp"),
            interface=capture_cfg.get("interface"),
        )
        return tcpdump
    return None


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
    host=None,
    capture_cfg=None,
) -> bool:
    # Only initialize logging if it hasn't been initialized already
    global _log_timestamp
    if _log_timestamp is None:
        init_test_logging()

    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]

    config_json = json.dumps(config, indent=4)

    log_info(f"Starting RxTxApp test: {get_case_id()}")
    log_to_file(f"Starting RxTxApp test: {get_case_id()}", host, build)
    log_to_file(f"Test configuration: {config_json}", host, build)

    remote_host = host
    remote_conn = remote_host.connection
    config_file = f"{build}/tests/config.json"
    f = remote_conn.path(config_file)
    json_content = config_json.replace('"', '\\"')
    f.write_text(json_content)
    config_path = os.path.join(build, config_file)
    log_to_file(f"Config file written to remote host: {config_path}", host, build)

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

    command = f"sudo {RXTXAPP_PATH} --config_file {config_path} --test_time {test_time}"

    if virtio_user:
        command += " --virtio_user"

    if rx_timing_parser:
        command += " --rx_timing_parser"

    if ptp:
        command += " --ptp"

    log_info(f"RxTxApp Command: {command}")
    log_to_file(f"RxTxApp Command: {command}", host, build)

    # Prepare tcpdump recorder if capture is enabled in the test configuration.
    tcpdump = prepare_tcpdump(capture_cfg, host)

    # For 4TX and 8k streams more timeout is needed
    timeout = test_time + 90
    if len(config["tx_sessions"]) >= 4 or any(
        format in str(config) for format in ["4320", "2160"]
    ):
        timeout = test_time + 120

    log_info(f"Running RxTxApp for {test_time} seconds with timeout {timeout}")
    log_to_file(
        f"Running RxTxApp for {test_time} seconds with timeout {timeout}", host, build
    )

    # Use run() for both local and remote
    cp = run(
        command,
        cwd=build,
        timeout=timeout,
        testcmd=True,
        host=remote_host,
    )

    # Start tcpdump capture (blocking, so it captures during traffic)
    try:
        if tcpdump:
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", 0.5))
            log_to_file("Started tcpdump capture", host, build)
    finally:
        cp.wait()

    # Check if process was killed or terminated unexpectedly
    if cp.return_code < 0:
        log_to_file(f"RxTxApp was killed with signal {-cp.return_code}", host, build)
        log_info(f"RxTxApp was killed with signal {-cp.return_code}")
        return False
    elif cp.return_code == 124:  # timeout return code
        log_to_file("RxTxApp timed out", host, build)
        log_info("RxTxApp timed out")
        return False
    elif cp.return_code == 137:  # SIGKILL
        log_to_file("RxTxApp was killed (SIGKILL)", host, build)
        log_info("RxTxApp was killed (SIGKILL)")
        return False
    elif cp.return_code == 143:  # SIGTERM
        log_to_file("RxTxApp was terminated (SIGTERM)", host, build)
        log_info("RxTxApp was terminated (SIGTERM)")
        return False

    # Get output lines
    output = cp.stdout_text.splitlines()
    for line in output:
        log_info(line)

    # Log the complete output to file
    log_to_file(f"RxTxApp Output:\n{cp.stdout_text}", host, build)

    if cp.return_code != 0:
        log_to_file(
            f"RxTxApp returned non-zero exit code: {cp.return_code}", host, build
        )
        if cp.stderr_text:
            log_to_file(f"RxTxApp stderr: {cp.stderr_text}", host, build)
        return False

    passed = True
    if len(config["tx_sessions"][0]["video"]) > 0:
        passed = passed and check_tx_output(
            config=config,
            output=output,
            session_type="video",
            fail_on_error=fail_on_error,
            host=host,
            build=build,
        )

    if len(config["rx_sessions"][0]["video"]) > 0:
        passed = passed and check_rx_output(
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

    log_info(f"RxTxApp test completed with result: {passed}")
    log_to_file(f"RxTxApp test completed with result: {passed}", host, build)

    return passed


def execute_perf_test(
    config: dict,
    build: str,
    test_time: int,
    fail_on_error: bool = True,
    capture_cfg=None,
    host=None,
) -> bool:
    # Only initialize logging if it hasn't been initialized already
    global _log_timestamp
    if _log_timestamp is None:
        init_test_logging()

    case_id = os.environ.get("PYTEST_CURRENT_TEST", "rxtxapp_test")
    case_id = case_id[: case_id.rfind("(") - 1] if "(" in case_id else case_id

    config_json = json.dumps(config, indent=4)

    log_info(f"Starting RxTxApp performance test: {get_case_id()}")
    log_to_file(f"Starting RxTxApp performance test: {get_case_id()}", host, build)
    log_to_file(f"Performance test configuration: {config_json}", host, build)

    remote_conn = host.connection
    config_file = f"{build}/tests/config.json"
    f = remote_conn.path(config_file)
    json_content = config_json.replace('"', '\\"')
    f.write_text(json_content)
    config_path = os.path.join(build, config_file)
    log_to_file(
        f"Performance config file written to remote host: {config_path}", host, build
    )

    command = f"sudo {RXTXAPP_PATH} --config_file {config_path} --test_time {test_time}"

    log_info(f"Performance RxTxApp Command: {command}")
    log_to_file(f"Performance RxTxApp Command: {command}", host, build)

    # Prepare tcpdump recorder if capture is enabled in the test configuration.
    tcpdump = prepare_tcpdump(capture_cfg, host)
    background = tcpdump is not None

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
        log_info(
            f"Scaling timeout for {total_replicas} replicas: +{additional_timeout}s"
        )
        log_to_file(
            f"Scaling timeout for {total_replicas} replicas: +{additional_timeout}s",
            host,
            build,
        )

    log_info(
        f"Running performance RxTxApp for {test_time} seconds with timeout {timeout}"
    )
    log_to_file(
        f"Running performance RxTxApp for {test_time} seconds with timeout {timeout}",
        host,
        build,
    )

    cp = run(
        command,
        cwd=build,
        timeout=timeout,
        testcmd=True,
        host=host,
        background=background,
    )

    # Start tcpdump capture (blocking, so it captures during traffic)
    try:
        if tcpdump:
            tcpdump.capture(capture_time=capture_cfg.get("capture_time", 0.5))
            log_to_file("Started performance test tcpdump capture", host, build)
    finally:
        cp.wait()

    # Enhanced logging for process completion
    log_info(
        f"Performance RxTxApp process completed with return code: {cp.return_code}"
    )
    log_to_file(
        f"Performance RxTxApp process completed with return code: {cp.return_code}",
        host,
        build,
    )

    # Check if process was killed or terminated unexpectedly
    if cp.return_code < 0:
        log_to_file(
            f"Performance RxTxApp was killed with signal {-cp.return_code}", host, build
        )
        log_info(f"Performance RxTxApp was killed with signal {-cp.return_code}")
        return False
    elif cp.return_code == 124:  # timeout return code
        log_to_file("Performance RxTxApp timed out", host, build)
        log_info("Performance RxTxApp timed out")
        return False
    elif cp.return_code == 137:  # SIGKILL
        log_to_file("Performance RxTxApp was killed (SIGKILL)", host, build)
        log_info("Performance RxTxApp was killed (SIGKILL)")
        return False
    elif cp.return_code == 143:  # SIGTERM
        log_to_file("Performance RxTxApp was terminated (SIGTERM)", host, build)
        log_info("Performance RxTxApp was terminated (SIGTERM)")
        return False

    # Get output lines
    output = cp.stdout_text.splitlines()
    for line in output:
        log_info(line)

    if cp.return_code != 0:
        log_to_file(
            f"Performance RxTxApp returned non-zero exit code: {cp.return_code}",
            host,
            build,
        )
        log_info(f"Performance RxTxApp returned non-zero exit code: {cp.return_code}")
        if cp.stderr_text:
            log_to_file(f"Performance RxTxApp stderr: {cp.stderr_text}", host, build)
            log_info(f"Performance RxTxApp stderr: {cp.stderr_text}")
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

    log_to_file(f"Performance test session type detected: {session_type}", host, build)

    result = check_tx_output(
        config=config,
        output=output,
        session_type=session_type,
        fail_on_error=fail_on_error,
        host=host,
        build=build,
    )

    log_info(f"Performance RxTxApp test completed with result: {result}")
    log_to_file(
        f"Performance RxTxApp test completed with result: {result}", host, build
    )

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
    log_info(f"Checking TX {session_type} output for OK results")
    if host:
        log_to_file(f"Checking TX {session_type} output for OK results", host, build)

    for line in output:
        if f"app_tx_{session_type}_result" in line and "OK" in line:
            ok_cnt += 1
            log_info(f"Found TX {session_type} OK result: {line}")
            if host:
                log_to_file(f"Found TX {session_type} OK result: {line}", host, build)

    replicas = 0
    for session in config["tx_sessions"]:
        if session[session_type]:
            for s in session[session_type]:
                replicas += s["replicas"]

    log_info(f"TX {session_type} check: {ok_cnt}/{replicas} OK results found")
    if host:
        log_to_file(
            f"TX {session_type} check: {ok_cnt}/{replicas} OK results found",
            host,
            build,
        )

    if ok_cnt == replicas:
        log_info(f"TX {session_type} check PASSED: all {replicas} sessions OK")
        if host:
            log_to_file(
                f"TX {session_type} check PASSED: all {replicas} sessions OK",
                host,
                build,
            )
        return True

    if fail_on_error:
        log_fail(f"tx {session_type} session failed")
        if host:
            log_to_file(
                f"TX {session_type} check FAILED: {ok_cnt}/{replicas} sessions OK",
                host,
                build,
            )
    else:
        log_info(f"tx {session_type} session failed")
        if host:
            log_to_file(
                f"TX {session_type} check FAILED (non-fatal): {ok_cnt}/{replicas} sessions OK",
                host,
                build,
            )

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
        log_info("Could not determine expected FPS from config")
        if host:
            log_to_file("Could not determine expected FPS from config", host, build)
        return False

    log_info(
        f"Checking TX FPS performance: expected {expected_fps} fps for {replicas} replicas"
    )
    if host:
        log_to_file(
            f"Checking TX FPS performance: expected {expected_fps} fps for {replicas} replicas",
            host,
            build,
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
    log_info(
        f"TX FPS performance check: {successful_count}/{replicas} sessions achieved target"
    )
    if host:
        log_to_file(
            f"TX FPS performance check: {successful_count}/{replicas} sessions achieved target",
            host,
            build,
        )

    if successful_count == replicas:
        if host:
            log_to_file(
                f"TX FPS performance check PASSED: all {replicas} sessions achieved target FPS",
                host,
                build,
            )
        return True

    if fail_on_error:
        log_fail(
            f"tx {session_type} fps performance failed: {successful_count}/{replicas} sessions"
        )
        if host:
            log_to_file(
                f"TX FPS performance check FAILED: {successful_count}/{replicas} sessions achieved target",
                host,
                build,
            )
    else:
        log_info(
            f"tx {session_type} fps performance failed: {successful_count}/{replicas} sessions"
        )
        if host:
            log_to_file(
                f"TX FPS performance check FAILED (non-fatal): {successful_count}/{replicas} sessions achieved target",
                host,
                build,
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
    log_info(f"Checking RX {session_type} output for OK results")
    if host:
        log_to_file(f"Checking RX {session_type} output for OK results", host, build)

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
            log_info(f"Found RX {session_type} OK result: {line}")
            if host:
                log_to_file(f"Found RX {session_type} OK result: {line}", host, build)

    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    log_info(f"RX {session_type} check: {ok_cnt}/{replicas} OK results found")
    if host:
        log_to_file(
            f"RX {session_type} check: {ok_cnt}/{replicas} OK results found",
            host,
            build,
        )

    if ok_cnt == replicas:
        log_info(f"RX {session_type} check PASSED: all {replicas} sessions OK")
        if host:
            log_to_file(
                f"RX {session_type} check PASSED: all {replicas} sessions OK",
                host,
                build,
            )
        return True

    if fail_on_error:
        log_fail(f"rx {session_type} session failed")
        if host:
            log_to_file(
                f"RX {session_type} check FAILED: {ok_cnt}/{replicas} sessions OK",
                host,
                build,
            )
    else:
        log_info(f"rx {session_type} session failed")
        if host:
            log_to_file(
                f"RX {session_type} check FAILED (non-fatal): {ok_cnt}/{replicas} sessions OK",
                host,
                build,
            )

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

    log_info(f"Checking TX {session_type} converter output")
    if host:
        log_to_file(
            f"Checking TX {session_type} converter output for format {transport_format}/{input_format}",
            host,
            build,
        )

    for line in output:
        if (
            f"st20p_tx_create({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, input fmt: {input_format}"
            in line
        ):
            ok_cnt += 1
            log_info(f"Found TX converter creation: {line}")
            if host:
                log_to_file(f"Found TX converter creation: {line}", host, build)

    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["tx_sessions"][0][session_type][0]["replicas"]

    log_info(
        f"TX {session_type} converter check: {ok_cnt}/{replicas} converters created"
    )
    if host:
        log_to_file(
            f"TX {session_type} converter check: {ok_cnt}/{replicas} converters created",
            host,
            build,
        )

    if ok_cnt == replicas:
        log_info(f"TX {session_type} converter check PASSED")
        if host:
            log_to_file(
                f"TX {session_type} converter check PASSED: all {replicas} converters created",
                host,
                build,
            )
        return True

    if fail_on_error:
        log_fail(f"tx {session_type} session failed")
        if host:
            log_to_file(
                f"TX {session_type} converter check FAILED: {ok_cnt}/{replicas} converters created",
                host,
                build,
            )
    else:
        log_info(f"tx {session_type} session failed")
        if host:
            log_to_file(
                f"TX {session_type} converter check FAILED (non-fatal): {ok_cnt}/{replicas} converters created",
                host,
                build,
            )

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

    log_info(f"Checking RX {session_type} converter output")
    if host:
        log_to_file(
            f"Checking RX {session_type} converter output for format {transport_format}/{output_format}",
            host,
            build,
        )

    for line in output:
        if (
            f"st20p_rx_create({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, output fmt {output_format}"
            in line
        ):
            ok_cnt += 1
            log_info(f"Found RX converter creation: {line}")
            if host:
                log_to_file(f"Found RX converter creation: {line}", host, build)

    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    log_info(
        f"RX {session_type} converter check: {ok_cnt}/{replicas} converters created"
    )
    if host:
        log_to_file(
            f"RX {session_type} converter check: {ok_cnt}/{replicas} converters created",
            host,
            build,
        )

    if ok_cnt == replicas:
        log_info(f"RX {session_type} converter check PASSED")
        if host:
            log_to_file(
                f"RX {session_type} converter check PASSED: all {replicas} converters created",
                host,
                build,
            )
        return True

    if fail_on_error:
        log_fail(f"rx {session_type} session failed")
        if host:
            log_to_file(
                f"RX {session_type} converter check FAILED: {ok_cnt}/{replicas} converters created",
                host,
                build,
            )
    else:
        log_info(f"rx {session_type} session failed")
        if host:
            log_to_file(
                f"RX {session_type} converter check FAILED (non-fatal): {ok_cnt}/{replicas} converters created",
                host,
                build,
            )

    return False


def check_and_set_ip(interface_name: str, ip_adress="192.168.17.102/24"):
    try:
        result = subprocess.run(
            ["ip", "addr", "show", interface_name], stdout=subprocess.PIPE, text=True
        )
        if ip_adress.split("/")[0] not in result.stdout:
            subprocess.run(
                ["sudo", "ip", "addr", "add", ip_adress, "dev", interface_name]
            )
            subprocess.run(["sudo", "ip", "link", "set", interface_name, "up"])
    except Exception:
        print(f"An error occured while trying to bind ip address to {interface_name}")


def check_and_bind_interface(interface_address: list, interface_mode="vf"):
    try:
        ifconfig_output = subprocess.check_output(["ifconfig"], universal_newlines=True)

        if "ens6f1" not in ifconfig_output and interface_mode == "pmd":
            subprocess.run(
                [
                    "sudo",
                    "/home/labrat/mtl/Media-Transport-Library/script/nicctl.sh",
                    "bind_pmd",
                    interface_address[0],
                ]
            )
            subprocess.run(
                [
                    "sudo",
                    "/home/labrat/mtl/Media-Transport-Library/script/nicctl.sh",
                    "disable_vf",
                    interface_address[1],
                ]
            )

        if "ens6f0" in ifconfig_output and interface_mode == "vf":
            subprocess.run(
                [
                    "sudo",
                    "/home/labrat/mtl/Media-Transport-Library/script/nicctl.sh",
                    "create_vf",
                    interface_address[0],
                ]
            )

    except subprocess.CalledProcessError as e:
        print(f"An error occured while running ifconfig: {e}")
    except Exception as e:
        print(f"An unexpected error occured while setting interface: {e}")


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


def log_to_file(message: str, host, build: str):
    """Log message to a file on the remote host"""
    global _log_timestamp

    # Initialize timestamp if not set
    if _log_timestamp is None:
        init_test_logging()

    test_name = sanitize_filename(get_case_id())
    log_file = f"{build}/tests/{test_name}_{_log_timestamp}_rxtxapp.log"

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
        f.write_text(current_content + log_entry)
    else:
        f.write_text(log_entry)


read_ip_addresses_from_json(json_filename)


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
    # Configure TX host interface only
    tx_config["interfaces"][0]["name"] = tx_nic_port_list[0]

    # Configure RX host interface only
    rx_config["interfaces"][0]["name"] = rx_nic_port_list[1]

    if test_mode == "unicast":
        tx_config["interfaces"][0]["ip"] = unicast_ip_dict["tx_interfaces"]
        rx_config["interfaces"][0]["ip"] = unicast_ip_dict["rx_interfaces"]
        tx_config["tx_sessions"][0]["dip"][0] = unicast_ip_dict[
            "rx_interfaces"
        ]  # TX sends to RX IP
        rx_config["rx_sessions"][0]["ip"][0] = unicast_ip_dict[
            "tx_interfaces"
        ]  # RX listens for TX IP
    elif test_mode == "multicast":
        tx_config["interfaces"][0]["ip"] = multicast_ip_dict["tx_interfaces"]
        rx_config["interfaces"][0]["ip"] = multicast_ip_dict["rx_interfaces"]
        tx_config["tx_sessions"][0]["dip"][0] = multicast_ip_dict["tx_sessions"]
        rx_config["rx_sessions"][0]["ip"][0] = multicast_ip_dict["rx_sessions"]
    elif test_mode == "kernel":
        tx_config["tx_sessions"][0]["dip"][0] = kernel_ip_dict["tx_sessions"]
        rx_config["rx_sessions"][0]["ip"][0] = kernel_ip_dict["rx_sessions"]
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
    capture_cfg=None,
) -> bool:
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]

    tx_config = config["tx_config"]
    rx_config = config["rx_config"]

    # Log test start
    log_info(f"Starting dual RxTxApp test: {get_case_id()}")
    log_to_file(f"Starting dual RxTxApp test: {get_case_id()}", tx_host, build)
    log_to_file(f"Starting dual RxTxApp test: {get_case_id()}", rx_host, build)
    log_to_file(f"TX config: {json.dumps(tx_config, indent=4)}", tx_host, build)
    log_to_file(f"RX config: {json.dumps(rx_config, indent=4)}", rx_host, build)

    # Prepare TX config
    tx_f = tx_host.connection.path(build, "tests", "tx_config.json")
    tx_json_content = tx_config.replace('"', '\\"')
    tx_f.write_text(tx_json_content)

    # Prepare RX config
    rx_f = tx_host.connection.path(build, "tests", "tx_config.json")
    rx_json_content = rx_config.replace('"', '\\"')
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

    tx_command = f"{base_command} --config_file {tx_config}"
    rx_command = f"{base_command} --config_file {rx_config}"

    log_info(f"TX Command: {tx_command}")
    log_info(f"RX Command: {rx_command}")
    log_to_file(f"TX Command: {tx_command}", tx_host, build)
    log_to_file(f"RX Command: {rx_command}", rx_host, build)

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

    # Get output from both hosts
    tx_output = tx_cp.stdout_text.splitlines()
    rx_output = rx_cp.stdout_text.splitlines()

    # Log outputs
    log_info("=== TX OUTPUT ===")
    log_to_file("=== TX OUTPUT ===", tx_host, build)
    for line in tx_output:
        log_info(line)
        log_to_file(line, tx_host, build)

    log_info("=== RX OUTPUT ===")
    log_to_file("=== RX OUTPUT ===", rx_host, build)
    for line in rx_output:
        log_info(line)
        log_to_file(line, rx_host, build)

    # Log the complete output to file
    log_to_file(f"TX RxTxApp Output:\n{tx_cp.stdout_text}", tx_host, build)
    log_to_file(f"RX RxTxApp Output:\n{rx_cp.stdout_text}", rx_host, build)

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

    log_info(f"Dual RxTxApp test completed with result: {passed}")
    log_to_file(f"Dual RxTxApp test completed with result: {passed}", tx_host, build)
    log_to_file(f"Dual RxTxApp test completed with result: {passed}", rx_host, build)

    return passed
