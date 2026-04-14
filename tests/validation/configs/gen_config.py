#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import argparse

import yaml


def gen_test_config(
    session_id: int,
    mtl_path: str,
    pci_device: str,
    ebu_ip: str,
    ebu_user: str,
    ebu_password: str,
) -> str:
    """Generate test_config for validation mode (EBU compliance + capture)."""
    pci_devices = [dev.strip() for dev in pci_device.split(",") if dev.strip()]
    if len(pci_devices) < 2:
        raise ValueError(
            "At least two PCI devices are required (e.g., '0000:4b:00.0,0000:4b:00.1'); "
            "the second device is used as sniff_pci_device"
        )

    test_config = {
        "session_id": session_id,
        "mtl_path": mtl_path,
        "media_path": "/mnt/media",
        "ramdisk": {
            "media": {"mountpoint": "/mnt/ramdisk/media", "size_gib": 16},
            "tmpfs_size_gib": 8,
            "pcap_dir": "/mnt/ramdisk/pcap",
        },
        "compliance": True,
        "capture_cfg": {
            "enable": True,
            "pcap_dir": "/mnt/ramdisk/pcap",
            "sniff_pci_device": pci_devices[1],
        },
        "ebu_server": {
            "ebu_ip": ebu_ip,
            "user": ebu_user,
            "password": ebu_password,
            "proxy": False,
        },
    }
    return yaml.safe_dump(test_config, sort_keys=False)


def gen_perf_test_config(
    session_id: int,
    mtl_path: str,
    media_path: str = "/mnt/media",
    test_time: int = 120,
) -> str:
    """Generate test_config for performance mode (no EBU/compliance/capture)."""
    test_config = {
        "session_id": session_id,
        "mtl_path": mtl_path,
        "media_path": media_path,
        "test_time": test_time,
        "ramdisk": {
            "media": {"mountpoint": "/mnt/ramdisk/media", "size_gib": 16},
            "tmpfs_size_gib": 8,
        },
        "compliance": False,
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
    """Build a single host entry for the topology config."""
    pci_devices = [dev.strip() for dev in pci_device.split(",")]
    network_interfaces = [
        {"pci_device": pci_dev, "interface_index": idx}
        for idx, pci_dev in enumerate(pci_devices)
    ]
    connection_options = {"port": 22, "username": username}
    # SSHConnection requires 'password' kwarg — use empty string when absent.
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
    pci_device: str,
    ip_address: str,
    username: str,
    password: str,
    key_path: str,
    extra_info: dict = None,
    second_host_pci_device: str = None,
    second_host_ip: str = None,
    second_host_username: str = None,
    second_host_password: str = None,
    second_host_key_path: str = None,
    second_host_extra_info: dict = None,
) -> str:
    hosts = []
    if second_host_ip and second_host_pci_device:
        hosts.append(
            _make_host(
                name="second_host",
                role="client",
                pci_device=second_host_pci_device,
                ip_address=second_host_ip,
                username=second_host_username or username,
                password=second_host_password or password,
                key_path=second_host_key_path or key_path,
                extra_info=second_host_extra_info or extra_info,
            )
        )
    hosts.append(
        _make_host(
            name="host",
            role="sut",
            pci_device=pci_device,
            ip_address=ip_address,
            username=username,
            password=password,
            key_path=key_path,
            extra_info=extra_info,
        )
    )
    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": hosts,
    }
    return yaml.safe_dump(topology_config, explicit_start=True, sort_keys=False)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate test and topology configs for the test framework."
    )
    parser.add_argument(
        "--mode",
        choices=["validation", "perf"],
        default="validation",
        help="Config mode: 'validation' (EBU/capture) or 'perf' (performance only)",
    )
    parser.add_argument(
        "--session_id",
        type=int,
        choices=range(0, 256),
        required=True,
        help="specify session ID (0-255)",
    )
    parser.add_argument(
        "--mtl_path",
        type=str,
        required=True,
        help="specify path to MTL directory",
    )
    parser.add_argument(
        "--pci_device",
        type=str,
        required=True,
        help="PCI BDF(s) of the NIC (comma-separated)",
    )
    parser.add_argument(
        "--ip_address",
        type=str,
        required=True,
        help="IP address of the SUT host",
    )
    parser.add_argument(
        "--username",
        type=str,
        required=True,
        help="SSH username for the SUT host",
    )
    parser.add_argument(
        "--password",
        type=str,
        default=None,
        help="SSH password for the SUT host",
    )
    parser.add_argument(
        "--key_path",
        type=str,
        default=None,
        help="SSH private key path for the SUT host",
    )
    # EBU args — required only in validation mode
    parser.add_argument("--ebu_ip", type=str, default=None, help="EBU LIST server IP")
    parser.add_argument("--ebu_user", type=str, default=None, help="EBU LIST username")
    parser.add_argument(
        "--ebu_password", type=str, default=None, help="EBU LIST password"
    )
    # Second host — required for dual-host perf tests
    parser.add_argument(
        "--second_host_ip", type=str, default=None, help="second host IP"
    )
    parser.add_argument(
        "--second_host_pci_device",
        type=str,
        default=None,
        help="second host PCI BDF(s)",
    )
    parser.add_argument(
        "--second_host_username",
        type=str,
        default=None,
        help="second host SSH username",
    )
    parser.add_argument(
        "--second_host_password",
        type=str,
        default=None,
        help="second host SSH password",
    )
    parser.add_argument(
        "--second_host_key_path",
        type=str,
        default=None,
        help="second host SSH key path",
    )
    parser.add_argument(
        "--second_host_mtl_path",
        type=str,
        default=None,
        help="MTL path on the second host (defaults to --mtl_path)",
    )
    # Perf-specific options
    parser.add_argument(
        "--test_time", type=int, default=120, help="test duration in seconds"
    )
    parser.add_argument(
        "--media_path", type=str, default="/mnt/media", help="path to media files"
    )

    args = parser.parse_args()
    if not args.password and not args.key_path:
        parser.error("one of the arguments --password --key_path is required")

    # ── Generate test_config ──
    if args.mode == "perf":
        test_config_yaml = gen_perf_test_config(
            session_id=args.session_id,
            mtl_path=args.mtl_path,
            media_path=args.media_path,
            test_time=args.test_time,
        )
    else:
        if not all([args.ebu_ip, args.ebu_user, args.ebu_password]):
            parser.error(
                "--ebu_ip, --ebu_user, --ebu_password are required in validation mode"
            )
        try:
            test_config_yaml = gen_test_config(
                session_id=args.session_id,
                mtl_path=args.mtl_path,
                pci_device=args.pci_device,
                ebu_ip=args.ebu_ip,
                ebu_user=args.ebu_user,
                ebu_password=args.ebu_password,
            )
        except ValueError as exc:
            parser.error(str(exc))

    # ── Generate topology_config ──
    extra_info = {"mtl_path": args.mtl_path}
    if args.mode == "perf":
        extra_info["media_path"] = args.media_path

    second_host_extra = dict(extra_info)
    if args.second_host_mtl_path:
        second_host_extra["mtl_path"] = args.second_host_mtl_path

    with open("test_config.yaml", "w") as file:
        file.write(test_config_yaml)
    with open("topology_config.yaml", "w") as file:
        file.write(
            gen_topology_config(
                pci_device=args.pci_device,
                ip_address=args.ip_address,
                username=args.username,
                password=args.password,
                key_path=args.key_path,
                extra_info=extra_info,
                second_host_pci_device=args.second_host_pci_device,
                second_host_ip=args.second_host_ip,
                second_host_username=args.second_host_username,
                second_host_password=args.second_host_password,
                second_host_key_path=args.second_host_key_path,
                second_host_extra_info=second_host_extra,
            )
        )


if __name__ == "__main__":
    main()
