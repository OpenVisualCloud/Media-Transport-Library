# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import copy
import json
import logging
import os
import re

from mtl_engine import udp_app_config

from .const import LOG_FOLDER
from .execute import call, log_fail, wait

logger = logging.getLogger(__name__)


def _resolve_udp_binary(build: str, relative_path: str) -> str:
    """Resolve a UDP test binary using .local_install or build directory.

    Checks (in order):
      1. .local_install/mtl/bin/<basename>  (CI mode)
      2. <build>/<relative_path>            (local build mode)
    Raises EnvironmentError if not found in either location.
    """
    basename = os.path.basename(relative_path)
    local_install = os.path.join(build, ".local_install", "mtl", "bin", basename)
    build_path = os.path.join(build, relative_path)

    if os.path.isfile(local_install) and os.access(local_install, os.X_OK):
        logger.debug(f"Resolved {basename} -> {local_install}")
        return local_install
    if os.path.isfile(build_path) and os.access(build_path, os.X_OK):
        logger.debug(f"Resolved {basename} -> {build_path}")
        return build_path
    raise EnvironmentError(
        f"Binary '{basename}' not found at '{local_install}' or '{build_path}'. "
        f"Build the project or ensure .local_install is populated."
    )


def _resolve_ld_preload(build: str) -> str:
    """Resolve libmtl_udp_preload.so path using env, .local_install, or system."""
    # Prefer env var set by CI
    env_path = os.environ.get("MTL_LD_PRELOAD")
    if env_path and os.path.isfile(env_path):
        return env_path
    # .local_install
    li_path = os.path.join(
        build,
        ".local_install",
        "mtl",
        "lib",
        "x86_64-linux-gnu",
        "libmtl_udp_preload.so",
    )
    if os.path.isfile(li_path):
        return li_path
    # System path
    sys_path = "/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so"
    if os.path.isfile(sys_path):
        return sys_path
    raise EnvironmentError(
        f"libmtl_udp_preload.so not found. Checked MTL_LD_PRELOAD env, "
        f"'{li_path}', and '{sys_path}'."
    )


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

    client_bin = _resolve_udp_binary(build, "build/app/UfdClientSample")
    server_bin = _resolve_udp_binary(build, "build/app/UfdServerSample")

    client_command = f"{client_bin} --p_tx_ip {sample_ip_dict['server']} --sessions_cnt {sessions_cnt}"
    server_command = f"{server_bin} --sessions_cnt {sessions_cnt}"

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
    send_env["LD_PRELOAD"] = _resolve_ld_preload(build)
    send_env["MUFD_CFG"] = os.path.join(os.getcwd(), send_config_file)

    receive_env = os.environ.copy()
    receive_env["LD_PRELOAD"] = _resolve_ld_preload(build)
    receive_env["MUFD_CFG"] = os.path.join(os.getcwd(), receive_config_file)

    send_bin = _resolve_udp_binary(
        build, "ecosystem/librist/librist/build/test/rist/test_send"
    )
    receive_bin = _resolve_udp_binary(
        build, "ecosystem/librist/librist/build/test/rist/test_receive"
    )

    send_command = (
        f"{send_bin} --sleep_us={sleep_us}"
        + f" --sleep_step={sleep_step} --dip={librist_ip_dict['receive']} --sessions_cnt={sessions_cnt}"
    )
    receive_command = (
        f"{receive_bin}"
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
