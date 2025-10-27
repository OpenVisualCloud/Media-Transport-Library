import argparse

import yaml


def gen_test_config(build: str, mtl_path: str) -> str:
    test_config = {
        "build": build,
        "mtl_path": mtl_path,
        "media_path": "/mnt/media",
        "ramdisk": {
            "media": {"mountpoint": "/mnt/ramdisk/media", "size_gib": 32},
            "pcap": {"mountpoint": "/mnt/ramdisk/pcap", "size_gib": 768},
        },
        "compliance": False,
        "capture_cfg": {"enable": False, "pcap_dir": "/mnt/ramdisk/pcap"},
        "ebu_server": {
            "ebu_ip": "ebu_ip",
            "user": "user",
            "password": "password",
            "proxy": False,
        },
    }
    return yaml.safe_dump(test_config, sort_keys=False)


def gen_topology_config(
    ip_address: str, username: str, password: str, key_path: str
) -> str:
    topology_config = {
        "metadata": {"version": "2.4"},
        "hosts": [
            {
                "name": "host",
                "instantiate": True,
                "role": "sut",
                "network_interfaces": [
                    {"pci_device": "8086:1592", "interface_index": 0}
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
        file.write(gen_test_config(build=args.build, mtl_path=args.mtl_path))
    with open("topology_config.yaml", "w") as file:
        file.write(
            gen_topology_config(
                ip_address=args.ip_address,
                username=args.username,
                password=args.password,
                key_path=args.key_path,
            )
        )


if __name__ == "__main__":
    main()
