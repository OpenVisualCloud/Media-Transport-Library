import argparse
import os

from compliance_client import PcapComplianceClient


def parse_args():
    parser = argparse.ArgumentParser(
        description="Upload a PCAP file to the EBU LIST server."
    )
    parser.add_argument(
        "--pcap",
        type=str,
        required=True,
        help="Path to the PCAP file to upload.",
    )
    parser.add_argument(
        "--ip",
        type=str,
        required=True,
        help="EBU LIST server IP address.",
    )
    parser.add_argument(
        "--user",
        type=str,
        required=True,
        help="Username for EBU LIST service.",
    )
    parser.add_argument(
        "--password",
        type=str,
        required=True,
        help="Password for EBU LIST service.",
    )
    parser.add_argument(
        "--proxy",
        type=str,
        required=False,
        help="Proxy for uploading to EBU LIST service.",
    )
    return parser.parse_args()


def upload_pcap(file_path, ip, login, password, proxies):
    # Check for login and password
    if not ip or not login or not password:
        raise Exception("IP address, login and password are required.")

    # Check if the file exists before proceeding
    if not os.path.isfile(file_path):
        raise Exception(f"File not found: {file_path}")

    # Create the uploader object and upload the PCAP file
    uploader = PcapComplianceClient(
        ebu_ip=ip, user=login, password=password, pcap_file=file_path, proxies=proxies
    )
    uuid = uploader.upload_pcap()  # Upload the PCAP file and get the UUID
    return uuid


if __name__ == "__main__":
    args = parse_args()
    # TODO: Handle proxies better, so each type can have a proper value
    proxies = None
    if args.proxy is not None:
        proxies = {"http": args.proxy, "https": args.proxy, "ftp": args.proxy}
    uuid = upload_pcap(args.pcap, args.ip, args.user, args.password, proxies)
    # Print extractable UUID
    print(f">>>UUID: {uuid}")
