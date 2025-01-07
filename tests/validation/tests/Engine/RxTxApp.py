# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
import copy
import json
import os
import re
import subprocess
import sys

from tests.Engine import rxtxapp_config
from tests.Engine.const import LOG_FOLDER
from tests.Engine.execute import log_fail, log_info, run

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
    config["rx_sessions"][0]["st30p"][0]["audio_url"] = filename
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


def execute_test(
    config: dict,
    build: str,
    test_time: int,
    fail_on_error: bool = True,
    virtio_user: bool = False,
    rx_timing_parser: bool = False,
    ptp: bool = False,
) -> bool:
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]
    config_file = os.path.join(LOG_FOLDER, "latest", f"{case_id}.json")

    save_as_json(json_file=config_file, content=config)

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

    command = f"sudo {RXTXAPP_PATH} --config_file {os.getcwd()}/{config_file} --test_time {test_time}"

    if virtio_user:
        command += " --virtio_user"

    if rx_timing_parser:
        command += " --rx_timing_parser"

    if ptp:
        command += " --ptp"

    cp = run(command, cwd=build, timeout=test_time + 90, testcmd=True)
    output = cp.stdout.splitlines()
    passed = True

    if len(config["tx_sessions"][0]["video"]) > 0:
        passed = passed and check_tx_output(
            config=config,
            output=output,
            session_type="video",
            fail_on_error=fail_on_error,
        )

    if len(config["rx_sessions"][0]["video"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="video",
            fail_on_error=fail_on_error,
        )

    if len(config["tx_sessions"][0]["st20p"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )
        passed = passed and check_tx_converter_output(
            config=config,
            output=output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )
        passed = passed and check_rx_converter_output(
            config=config,
            output=output,
            session_type="st20p",
            fail_on_error=fail_on_error,
        )

    if len(config["tx_sessions"][0]["st22p"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="st22p",
            fail_on_error=fail_on_error,
        )

    if len(config["tx_sessions"][0]["audio"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="audio",
            fail_on_error=fail_on_error,
        )

    if len(config["tx_sessions"][0]["ancillary"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="anc",
            fail_on_error=fail_on_error,
        )

    if len(config["tx_sessions"][0]["fastmetadata"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="fastmetadata",
            fail_on_error=fail_on_error,
        )

    if len(config["tx_sessions"][0]["st30p"]) > 0:
        passed = passed and check_rx_output(
            config=config,
            output=output,
            session_type="st30p",
            fail_on_error=fail_on_error,
        )

    return passed


def execute_perf_test(
    config: dict,
    build: str,
    test_time: int,
    fail_on_error: bool = True,
) -> bool:
    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]
    config_file = os.path.join(LOG_FOLDER, "latest", f"{case_id}.json")

    save_as_json(json_file=config_file, content=config)

    command = f"sudo {RXTXAPP_PATH} --config_file {os.getcwd()}/{config_file} --test_time {test_time}"

    # For 4TX and 8k streams more timeout is needed
    cp = run(command, cwd=build, timeout=test_time + 120, testcmd=True)
    output = cp.stdout.splitlines()

    return check_tx_output(
        config=config, output=output, session_type="video", fail_on_error=fail_on_error
    )


def save_as_json(json_file: str, content: dict):
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
    config: dict, output: list, session_type: str, fail_on_error: bool
) -> bool:
    ok_cnt = 0

    for line in output:
        if f"app_tx_{session_type}_result" in line and "OK" in line:
            ok_cnt += 1

    replicas = 0

    for session in config["tx_sessions"]:
        if session[session_type]:
            for s in session[session_type]:
                replicas += s["replicas"]

    log_info(f"%%% DEBUG %%% {replicas} replicas")

    if ok_cnt == replicas:
        return True

    if fail_on_error:
        log_fail(f"tx {session_type} session failed")
    else:
        log_info(f"tx {session_type} session failed")

    return False


def check_rx_output(
    config: dict, output: list, session_type: str, fail_on_error: bool
) -> bool:
    ok_cnt = 0

    pattern = re.compile(r"app_rx_.*_result")

    for line in output:
        if pattern.search(line) and "OK" in line:
            ok_cnt += 1
    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    if ok_cnt == replicas:
        return True

    if fail_on_error:
        log_fail(f"rx {session_type} session failed")
    else:
        log_info(f"rx {session_type} session failed")

    return False


def check_tx_converter_output(
    config: dict, output: list, session_type: str, fail_on_error: bool
) -> bool:
    ok_cnt = 0
    transport_format = config["tx_sessions"][0]["st20p"][0]["transport_format"]
    input_format = config["tx_sessions"][0]["st20p"][0]["input_format"]
    for line in output:
        if (
            f"st20p_tx_create({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, input fmt: {input_format}"
            in line
        ):
            ok_cnt += 1
    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["tx_sessions"][0][session_type][0]["replicas"]

    if ok_cnt == replicas:
        return True

    if fail_on_error:
        log_fail(f"tx {session_type} session failed")
    else:
        log_info(f"tx {session_type} session failed")

    return False


def check_rx_converter_output(
    config: dict, output: list, session_type: str, fail_on_error: bool
) -> bool:
    ok_cnt = 0

    transport_format = config["rx_sessions"][0]["st20p"][0]["transport_format"]
    output_format = config["rx_sessions"][0]["st20p"][0]["output_format"]

    for line in output:
        if (
            f"st20p_rx_create({ok_cnt}), transport fmt ST20_FMT_{transport_format.upper()}, output fmt {output_format}"
            in line
        ):
            ok_cnt += 1
    if session_type == "anc":
        session_type = "ancillary"
    replicas = config["rx_sessions"][0][session_type][0]["replicas"]

    if ok_cnt == replicas:
        return True

    if fail_on_error:
        log_fail(f"rx {session_type} session failed")
    else:
        log_info(f"rx {session_type} session failed")

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


read_ip_addresses_from_json(json_filename)
