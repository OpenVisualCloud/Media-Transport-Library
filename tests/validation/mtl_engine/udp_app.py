# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import copy
import json
import os
import re

from mtl_engine import udp_app_config

from .const import LOG_FOLDER
from .execute import call, log_fail, wait

sample_ip_dict = {
    "client": "192.168.106.10",
    "server": "192.168.106.11",
}

librist_ip_dict = {
    "send": "192.168.106.10",
    "receive": "192.168.106.11",
}


def execute_test_sample(
    build: str,
    test_time: int,
    nic_port_list: list,
    sessions_cnt: int,
    host=None,
) -> None:
    clinet_config = create_config(
        config=copy.deepcopy(udp_app_config.config_client),
        port=nic_port_list[0],
        ip=sample_ip_dict["client"],
        netmask="255.255.255.0",
    )
    server_config = create_config(
        config=copy.deepcopy(udp_app_config.config_server),
        port=nic_port_list[1],
        ip=sample_ip_dict["server"],
        netmask="255.255.255.0",
    )

    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]

    client_config_file = os.path.join(LOG_FOLDER, "latest", f"{case_id}_client.json")
    server_config_file = os.path.join(LOG_FOLDER, "latest", f"{case_id}_server.json")

    save_as_json(config_file=client_config_file, config=clinet_config)
    save_as_json(config_file=server_config_file, config=server_config)

    client_env = os.environ.copy()
    client_env["MUFD_CFG"] = os.path.join(os.getcwd(), client_config_file)

    server_env = os.environ.copy()
    server_env["MUFD_CFG"] = os.path.join(os.getcwd(), server_config_file)

    client_command = f"./build/app/UfdClientSample --p_tx_ip {sample_ip_dict['server']} --sessions_cnt {sessions_cnt}"
    server_command = f"./build/app/UfdServerSample --sessions_cnt {sessions_cnt}"

    client_proc = call(client_command, build, test_time, sigint=True, env=client_env)
    server_proc = call(server_command, build, test_time, sigint=True, env=server_env)

    # Wait for both processes to finish
    wait(client_proc)
    wait(server_proc)

    if not check_received_packets(client_proc.output) or not check_received_packets(
        server_proc.output
    ):
        log_fail("Received less than 99% sent packets")


def execute_test_librist(
    build: str,
    test_time: int,
    nic_port_list: list,
    sleep_us: int,
    sleep_step: int,
    sessions_cnt: int,
    host=None,
) -> None:
    send_config = create_config(
        config=copy.deepcopy(udp_app_config.config_librist_send),
        port=nic_port_list[0],
        ip=librist_ip_dict["send"],
    )
    receive_config = create_config(
        config=copy.deepcopy(udp_app_config.config_librist_receive),
        port=nic_port_list[1],
        ip=librist_ip_dict["receive"],
    )

    case_id = os.environ["PYTEST_CURRENT_TEST"]
    case_id = case_id[: case_id.rfind("(") - 1]

    send_config_file = os.path.join(LOG_FOLDER, "latest", f"{case_id}_send.json")
    receive_config_file = os.path.join(LOG_FOLDER, "latest", f"{case_id}_receive.json")

    save_as_json(config_file=send_config_file, config=send_config)
    save_as_json(config_file=receive_config_file, config=receive_config)

    send_env = os.environ.copy()
    send_env["LD_PRELOAD"] = "/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so"
    send_env["MUFD_CFG"] = os.path.join(os.getcwd(), send_config_file)

    receive_env = os.environ.copy()
    receive_env["LD_PRELOAD"] = "/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so"
    receive_env["MUFD_CFG"] = os.path.join(os.getcwd(), receive_config_file)

    send_command = (
        f"./ecosystem/librist/librist/build/test/rist/test_send --sleep_us={sleep_us}"
        + f" --sleep_step={sleep_step} --dip={librist_ip_dict['receive']} --sessions_cnt={sessions_cnt}"
    )
    receive_command = (
        "./ecosystem/librist/librist/build/test/rist/test_receive"
        + f" --bind_ip={librist_ip_dict['receive']} --sessions_cnt={sessions_cnt}"
    )

    send_proc = call(send_command, build, test_time, sigint=True, env=send_env)
    receive_proc = call(receive_command, build, test_time, sigint=True, env=receive_env)

    wait(send_proc)
    wait(receive_proc)

    if not check_connected_receivers(
        send_proc.output, sessions_cnt
    ) or not check_connected_receivers(receive_proc.output, sessions_cnt):
        log_fail("Wrong number of connected receivers")


def create_config(config: dict, port: str, ip: str, netmask: str = None) -> None:
    config = copy.deepcopy(config)
    config["interfaces"][0]["port"] = port
    config["interfaces"][0]["ip"] = ip

    if netmask:
        config["interfaces"][0]["netmask"] = netmask

    return config


def save_as_json(config_file: str, config: dict):
    with open(config_file, "w") as file:
        json.dump(config, file, indent=4)


def check_received_packets(stdout: str) -> bool:
    pattern = re.compile(r"ufd_.*_status\(0\), send (\d+) pkts.* recv (\d+) pkts")
    matches = 0

    for line in stdout.splitlines():
        match = pattern.search(line)

        if match:
            matches += 1
            send_packets = int(match.group(1))
            recv_packets = int(match.group(2))

            if send_packets * 0.99 >= recv_packets:
                return False

    if matches == 0:
        return False

    return True


def check_connected_receivers(stdout: str, sessions_cnt: int) -> bool:
    pattern = re.compile(r".*\((\d+)\): Peer \d+ receiver with name \d+ reconnected")
    numbers = set(range(0, sessions_cnt))

    for line in stdout.splitlines():
        match = pattern.search(line)

        if match:
            receiver_number = int(match.group(1))
            numbers.remove(receiver_number)

    if len(numbers) == 0:
        return True

    return False
