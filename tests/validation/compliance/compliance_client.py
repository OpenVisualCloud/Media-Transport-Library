import logging
import os
import time

import requests

logger = logging.getLogger(__name__)


class PcapComplianceClient:
    def __init__(
        self,
        ebu_ip,
        user,
        password,
        pcap_file=None,
        pcap_id=None,
        proxies={"http": "", "https": "", "ftp": ""},
    ):
        """
        Initialize the client.
        """
        self.ebu_ip = ebu_ip
        self.user = user
        self.password = password
        self.pcap_file = pcap_file
        self.proxies = proxies
        self.pcap_id = pcap_id
        self.token = None
        self.session = requests.Session()
        self.session.trust_env = False  # Do not use system proxy settings
        self.authenticate()

    def authenticate(self):
        """
        Authenticate with the EBU server and store the access token.
        """
        url = f"http://{self.ebu_ip}/auth/login"
        headers = {"Content-Type": "application/json"}
        data = {"username": self.user, "password": self.password}
        response = self.session.post(
            url, headers=headers, json=data, verify=False, proxies=self.proxies
        )
        response.raise_for_status()
        self.token = response.json().get("content", {}).get("token")
        if not self.token:
            raise Exception("Authentication failed: No token received.")

    def upload_pcap(self):
        """
        Upload the PCAP file to the EBU server and store the returned UUID.
        Returns the UUID of the uploaded PCAP.
        """
        url = f"http://{self.ebu_ip}/api/pcap"
        headers = {"Authorization": f"Bearer {self.token}"}
        if self.pcap_file:
            with open(self.pcap_file, "rb") as f:
                files = {
                    "pcap": (
                        os.path.basename(self.pcap_file),
                        f,
                        "application/vnd.tcpdump.pcap",
                    )
                }
                response = self.session.put(
                    url,
                    headers=headers,
                    files=files,
                    verify=False,
                    proxies=self.proxies,
                )
            response.raise_for_status()
            self.pcap_id = response.json().get("uuid")
        if not self.pcap_id:
            raise Exception("Upload failed: No UUID received.")
        return self.pcap_id

    def download_report(self, retries=10):
        """
        Download the compliance report for the uploaded PCAP file.
        returns the report as a JSON object.
        """
        if not self.pcap_id:
            raise ValueError("No PCAP ID available to download report.")
        if retries is None or retries <= 0:
            logger.error(
                "Invalid retries value (%s), skipping compliance check", retries
            )
            return False
        url = f"http://{self.ebu_ip}/api/pcap/{self.pcap_id}/report?type=json"
        headers = {"Authorization": f"Bearer {self.token}"}
        initial_retries = retries
        while retries > 0:
            response = self.session.get(
                url, headers=headers, verify=False, proxies=self.proxies
            )

            # EBU LIST may return 404 until the report is generated.
            if response.status_code == 404:
                time.sleep(1)
                retries -= 1
                continue

            response.raise_for_status()
            report = response.json()
            if report.get("analyzed", False):
                return report

            time.sleep(1)
            retries -= 1

        logger.error(
            "Report is not ready after %s attempts, skipping compliance check",
            initial_retries,
        )
        return False

    def check_compliance(self, report=None):
        """
        Check the compliance result from the downloaded report.
        Returns True if compliant, False otherwise.
        """
        if report is None:
            report = self.download_report()
        is_compliant = report.get("not_compliant_streams", 1) == 0
        if is_compliant:
            is_compliant = not any(
                [
                    1 if stream.get("media_type") == "unknown" else 0
                    for stream in report.get("streams", [])
                ]
            )
        return is_compliant, report

    def delete_pcap(self, pcap_id=None):
        """
        Delete the PCAP file and its report from the EBU server.
        If pcap_id is not provided, uses self.pcap_id.
        """
        if pcap_id is None:
            pcap_id = self.pcap_id
        if not pcap_id:
            raise ValueError("No PCAP ID provided for deletion.")
        url = f"http://{self.ebu_ip}/api/pcap/{pcap_id}"
        headers = {"Authorization": f"Bearer {self.token}"}
        response = self.session.delete(
            url, headers=headers, verify=False, proxies=self.proxies
        )
        if response.status_code == 200:
            logger.info(f"PCAP {pcap_id} deleted successfully from EBU server.")
            return True
        else:
            logger.error(
                f"Failed to delete PCAP {pcap_id}: {response.status_code} {response.text}"
            )
            return False
