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


def gen_topology_config(
    pci_device: str, ip_address: str, username: str, password: str, key_path: str
) -> str:
    # Support comma-separated PCI devices for multiple interfaces
    pci_devices = [dev.strip() for dev in pci_device.split(",")]

    network_interfaces = [
        {
            "pci_device": pci_dev,
            "interface_index": idx,
        }
        for idx, pci_dev in enumerate(pci_devices)
    ]

    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": [
            {
                "name": "host",
                "instantiate": True,
                "role": "sut",
                "network_interfaces": network_interfaces,
                "connections": [
                    {
                        "ip_address": ip_address,
                        "connection_type": "SSHConnection",
                        "connection_options": {
                            "port": 22,
                            "username": username,
                            "password": password,
                        },
                    }
                ],
            }
        ],
    }
    if key_path != "None":
        topology_config["hosts"][0]["connections"][0]["connection_options"][
            "key_path"
        ] = key_path
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
            )
        )


if __name__ == "__main__":
    main()
