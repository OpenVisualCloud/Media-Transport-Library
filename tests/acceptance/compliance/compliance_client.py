import logging
import os
import time

import requests

logger = logging.getLogger(__name__)


def no_verdict_reason(report):
    """Return why *report* carries no compliance verdict, or None if it has one.

    Neither shape may fall through to the ordinary verdict: an analyzed report
    with no streams has ``not_compliant_streams == 0``, which would otherwise
    read as "compliant" about a capture that held nothing to judge.
    """
    if not report:
        return (
            "EBU LIST report is unavailable or was never analyzed, so compliance "
            "was not evaluated."
        )
    if not report.get("streams"):
        return (
            "EBU LIST analyzed the capture but found no ST 2110 streams in it, so "
            "there was no compliance verdict to give. Most often the transmitter "
            "was not running for the capture window, the sniffer port does not "
            "see the stream, or the capture filter's destination IP does not "
            "match it."
        )
    return None


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
            "Report is unavailable or not analyzed after %s attempts",
            initial_retries,
        )
        return False

    def check_compliance(self, report=None):
        """
        Check the compliance result from the downloaded report.
        Returns ``(verdict, report)``, the verdict being True if compliant,
        False if non-compliant, and None when the report carries no verdict at
        all -- see :func:`no_verdict_reason`.
        """
        if report is None:
            report = self.download_report()

        reason = no_verdict_reason(report)
        if reason:
            logger.error("%s", reason)
            return None, report

        streams = report["streams"]
        not_compliant_streams = report.get("not_compliant_streams", 1)
        unknown_media_streams = [
            (idx, stream)
            for idx, stream in enumerate(streams)
            if stream.get("media_type") == "unknown"
        ]

        is_compliant = not_compliant_streams == 0
        if is_compliant and unknown_media_streams:
            is_compliant = False

        if not is_compliant:
            if not_compliant_streams and not_compliant_streams > 0:
                logger.warning(
                    "Compliance report indicates non-compliance: not_compliant_streams=%s",
                    not_compliant_streams,
                )
            if unknown_media_streams:
                stream_refs = []
                for idx, stream in unknown_media_streams:
                    stream_id = (
                        stream.get("id")
                        or stream.get("stream_id")
                        or stream.get("uuid")
                        or idx
                    )
                    stream_refs.append(str(stream_id))
                logger.warning(
                    "Compliance report indicates non-compliance: %s stream(s) with unknown media_type (ids=%s)",
                    len(unknown_media_streams),
                    ", ".join(stream_refs),
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
