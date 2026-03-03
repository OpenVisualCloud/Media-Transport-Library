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
    # Support comma-separated PCI devices for multiple interfaces.
    # The capture sniff interface must be explicitly provided as the second device.
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


def _make_host(
    name, role, pci_device, ip_address, username, password, key_path, mtl_path=None
):
    """Build a single host entry for the topology config."""
    pci_devices = [dev.strip() for dev in pci_device.split(",")]
    network_interfaces = [
        {"pci_device": pci_dev, "interface_index": idx}
        for idx, pci_dev in enumerate(pci_devices)
    ]
    connection_options = {
        "port": 22,
        "username": username,
        "password": password,
    }
    if key_path != "None":
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
    if mtl_path:
        host["extra_info"] = {"mtl_path": mtl_path}
    return host


def gen_topology_config(
    pci_device: str,
    ip_address: str,
    username: str,
    password: str,
    key_path: str,
    second_host_pci_device: str = None,
    second_host_ip: str = None,
    second_host_username: str = None,
    second_host_password: str = None,
    second_host_key_path: str = None,
    mtl_path: str = None,
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
                mtl_path=mtl_path,
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
            mtl_path=mtl_path,
        )
    )
    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": hosts,
    }
    return yaml.safe_dump(topology_config, explicit_start=True, sort_keys=False)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate example test and topology configs for the test framework."
    )
    parser.add_argument(
        "--session_id",
        type=int,
        choices=range(0, 256),
        required=True,
        help="specify session ID (0 - 255)",
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
        help="specify PCI BDF(s) of the NIC (comma-separated for multiple interfaces, e.g., \
            '0000:4b:00.0,0000:4b:00.1'); the second device is used for capture sniffing",
    )
    parser.add_argument(
        "--ebu_ip",
        type=str,
        required=True,
        help="EBU LIST server IP/hostname (RUNNER_EBU_LIST_IP)",
    )
    parser.add_argument(
        "--ebu_user",
        type=str,
        required=True,
        help="EBU LIST username (RUNNER_EBU_LIST_USER)",
    )
    parser.add_argument(
        "--ebu_password",
        type=str,
        required=True,
        help="EBU LIST password (RUNNER_EBU_LIST_PASSWORD)",
    )
    parser.add_argument(
        "--ip_address",
        type=str,
        required=True,
        help="specify IP address of the test host",
    )
    parser.add_argument(
        "--username",
        type=str,
        required=True,
        help="specify username for the test host",
    )
    parser.add_argument(
        "--password",
        type=str,
        default="None",
        help="specify password for the test host",
    )
    parser.add_argument(
        "--key_path",
        type=str,
        default="None",
        help="specify path to SSH private key for the test host",
    )
    parser.add_argument(
        "--second_host_ip",
        type=str,
        default=None,
        help="SSH IP of the second host (enables dual-host topology)",
    )
    parser.add_argument(
        "--second_host_pci_device",
        type=str,
        default=None,
        help="PCI BDF(s) of the second host NIC (comma-separated)",
    )
    parser.add_argument(
        "--second_host_username",
        type=str,
        default=None,
        help="SSH username for the second host (defaults to --username)",
    )
    parser.add_argument(
        "--second_host_password",
        type=str,
        default=None,
        help="SSH password for the second host (defaults to --password)",
    )
    parser.add_argument(
        "--second_host_key_path",
        type=str,
        default=None,
        help="SSH key path for the second host (defaults to --key_path)",
    )
    args = parser.parse_args()
    if args.password == "None" and args.key_path == "None":
        parser.error("one of the arguments --password --key_path is required")

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
                second_host_pci_device=args.second_host_pci_device,
                second_host_ip=args.second_host_ip,
                second_host_username=args.second_host_username,
                second_host_password=args.second_host_password,
                second_host_key_path=args.second_host_key_path,
                mtl_path=args.mtl_path,
            )
        )


if __name__ == "__main__":
    main()
