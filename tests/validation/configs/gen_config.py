import argparse

import yaml


def gen_test_config(
    session_id: int,
    build: str,
    mtl_path: str,
    pci_device: str,
    ebu_ip: str,
    ebu_user: str,
    ebu_password: str,
) -> str:
    test_config = {
        "session_id": session_id,
        "build": build,
        "mtl_path": mtl_path,
        "media_path": "/mnt/media",
        "ramdisk": {
            "media": {"mountpoint": "/mnt/ramdisk/media", "size_gib": 32},
            "tmpfs_size_gib": 12,
            "pcap_dir": "/mnt/ramdisk/pcap",
        },
        "compliance": True,
        "capture_cfg": {
            "enable": True,
            "pcap_dir": "/mnt/ramdisk/pcap",
            "sniff_pci_device": pci_device,
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
    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": [
            {
                "name": "host",
                "instantiate": True,
                "role": "sut",
                "network_interfaces": [
                    {
                        "pci_device": pci_device,
                        "interface_index": 0,
                    },
                    {
                        "pci_device": pci_device,
                        "interface_index": 1,
                    }
                ],
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
        "--build",
        type=str,
        required=True,
        help="specify path to MTL directory",
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
        help="specify PCI ID of the NIC",
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
    with open("test_config.yaml", "w") as file:
        file.write(
            gen_test_config(
                session_id=args.session_id,
                build=args.build,
                mtl_path=args.mtl_path,
                pci_device=args.pci_device,
                ebu_ip=args.ebu_ip,
                ebu_user=args.ebu_user,
                ebu_password=args.ebu_password,
            )
        )
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
