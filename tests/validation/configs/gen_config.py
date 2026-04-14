#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import argparse

import yaml


def gen_test_config(
    session_id: int,
    mtl_path: str,
    pci_device: str,
    ebu_ip: str = None,
    ebu_user: str = None,
    ebu_password: str = None,
    media_path: str = "/mnt/media",
    test_time: int = 120,
) -> str:
    pci_devices = [dev.strip() for dev in pci_device.split(",") if dev.strip()]

    test_config = {
        "session_id": session_id,
        "mtl_path": mtl_path,
        "media_path": media_path,
        "test_time": test_time,
        "ramdisk": {
            "media": {"mountpoint": "/mnt/ramdisk/media", "size_gib": 16},
            "tmpfs_size_gib": 8,
        },
    }

    has_ebu = all([ebu_ip, ebu_user, ebu_password])
    has_sniff = len(pci_devices) >= 2
    test_config["compliance"] = has_ebu and has_sniff

    if has_sniff:
        test_config["ramdisk"]["pcap_dir"] = "/mnt/ramdisk/pcap"
        test_config["capture_cfg"] = {
            "enable": True,
            "pcap_dir": "/mnt/ramdisk/pcap",
            "sniff_pci_device": pci_devices[1],
        }
    if has_ebu:
        test_config["ebu_server"] = {
            "ebu_ip": ebu_ip,
            "user": ebu_user,
            "password": ebu_password,
            "proxy": False,
        }

    return yaml.safe_dump(test_config, sort_keys=False)


def _make_host(
    name: str,
    role: str,
    pci_device: str,
    ip_address: str,
    username: str,
    password: str,
    key_path: str,
    extra_info: dict = None,
) -> dict:
    pci_devices = [dev.strip() for dev in pci_device.split(",")]
    network_interfaces = [
        {"pci_device": pci_dev, "interface_index": idx}
        for idx, pci_dev in enumerate(pci_devices)
    ]
    connection_options = {"port": 22, "username": username}
    connection_options["password"] = password if password else ""
    if key_path:
        connection_options["key_path"] = key_path
    host = {
        "name": name,
        "instantiate": True,
        "role": role,
        "network_interfaces": network_interfaces,
        "connections": [
            {
                "ip_address": ip_address,
                "connection_type": "SSHConnection",
                "connection_options": connection_options,
            }
        ],
    }
    if extra_info:
        host["extra_info"] = extra_info
    return host


def gen_topology_config(
    pci_devices: list[str],
    ip_addresses: list[str],
    usernames: list[str],
    passwords: list[str],
    key_paths: list[str],
    mtl_paths: list[str],
    media_path: str = "/mnt/media",
) -> str:
    n = len(ip_addresses)
    hosts = []
    for i in range(n):
        is_sut = i == n - 1
        extra_info = {"mtl_path": mtl_paths[i], "media_path": media_path}
        hosts.append(
            _make_host(
                name="host" if is_sut else f"host_{i}",
                role="sut" if is_sut else "client",
                pci_device=pci_devices[i],
                ip_address=ip_addresses[i],
                username=usernames[i],
                password=passwords[i],
                key_path=key_paths[i],
                extra_info=extra_info,
            )
        )

    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": hosts,
    }
    return yaml.safe_dump(topology_config, explicit_start=True, sort_keys=False)


def _extend_list(lst: list, n: int) -> list:
    """Extend a list to length n by repeating the last element."""
    return (lst + [lst[-1]] * (n - len(lst)))[:n]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate test and topology configs for the test framework."
    )
    parser.add_argument(
        "--session_id",
        type=int,
        choices=range(0, 256),
        required=True,
        help="session ID (0-255)",
    )
    parser.add_argument(
        "--mtl_path",
        type=str,
        nargs="+",
        required=True,
        help="MTL path(s) — one per host",
    )
    parser.add_argument(
        "--pci_device",
        type=str,
        nargs="+",
        required=True,
        help="PCI BDF(s) per host (comma-separated within each host)",
    )
    parser.add_argument(
        "--ip_address",
        type=str,
        nargs="+",
        required=True,
        help="IP address(es) — one per host",
    )
    parser.add_argument(
        "--username",
        type=str,
        nargs="+",
        required=True,
        help="SSH username(s) — one per host",
    )
    parser.add_argument("--password", type=str, nargs="+", default=[None])
    parser.add_argument("--key_path", type=str, nargs="+", default=[None])
    # Optional EBU args
    parser.add_argument("--ebu_ip", type=str, default=None)
    parser.add_argument("--ebu_user", type=str, default=None)
    parser.add_argument("--ebu_password", type=str, default=None)
    # Optional test settings
    parser.add_argument("--test_time", type=int, default=120)
    parser.add_argument("--media_path", type=str, default="/mnt/media")

    args = parser.parse_args()

    n = len(args.ip_address)
    passwords = _extend_list(args.password, n)
    key_paths = _extend_list(args.key_path, n)
    mtl_paths = _extend_list(args.mtl_path, n)
    pci_devices = _extend_list(args.pci_device, n)
    usernames = _extend_list(args.username, n)

    if not any(passwords) and not any(key_paths):
        parser.error("at least one of --password or --key_path is required")

    test_config_yaml = gen_test_config(
        session_id=args.session_id,
        mtl_path=mtl_paths[-1],
        pci_device=pci_devices[-1],
        ebu_ip=args.ebu_ip,
        ebu_user=args.ebu_user,
        ebu_password=args.ebu_password,
        media_path=args.media_path,
        test_time=args.test_time,
    )

    with open("test_config.yaml", "w") as file:
        file.write(test_config_yaml)
    with open("topology_config.yaml", "w") as file:
        file.write(
            gen_topology_config(
                pci_devices=pci_devices,
                ip_addresses=args.ip_address,
                usernames=usernames,
                passwords=passwords,
                key_paths=key_paths,
                mtl_paths=mtl_paths,
                media_path=args.media_path,
            )
        )


if __name__ == "__main__":
    main()
