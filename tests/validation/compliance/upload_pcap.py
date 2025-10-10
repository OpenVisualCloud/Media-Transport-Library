import argparse
import os

if __name__ == "__main__":
    from pcap_compliance import PcapComplianceClient
else:
    from compliance.pcap_compliance import PcapComplianceClient


def parse_args():
    parser = argparse.ArgumentParser(
        description="Upload a PCAP file to the EBU server."
    )
    parser.add_argument(
        "--pcap_file_path",
        type=str,
        required=True,
        help="Full path to the PCAP file to upload.",
    )
    parser.add_argument(
        "--ip", type=str, required=True, help="IP address to the EBU LIST service."
    )
    parser.add_argument(
        "--login", type=str, required=True, help="Login to the EBU LIST service."
    )
    parser.add_argument(
        "--password", type=str, required=True, help="Password to the EBU LIST service."
    )
    parser.add_argument(
        "--proxy",
        type=str,
        required=False,
        help="Proxy used to upload to the EBU LIST service.",
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
    # Empty config_path to avoid loading from non-existing YAML file
    uploader = PcapComplianceClient(
        pcap_file=file_path, config_path="", proxies=proxies
    )
    uploader.ebu_ip = ip
    uploader.user = login
    uploader.password = password
    uploader.authenticate()  # Authenticate with the EBU server
    uuid = uploader.upload_pcap()  # Upload the PCAP file and get the UUID

    return uuid


if __name__ == "__main__":
    args = parse_args()
    # TODO: Handle proxies better, so each type can have a proper value
    proxies = None
    if args.proxy is not None:
        proxies = {"http": args.proxy, "https": args.proxy, "ftp": args.proxy}
    uuid = upload_pcap(args.pcap_file_path, args.ip, args.login, args.password, proxies)
    # Print extractable UUID
    print(f">>>UUID>>>{uuid}<<<UUID<<<")
