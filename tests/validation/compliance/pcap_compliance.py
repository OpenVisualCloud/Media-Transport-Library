import datetime
import json
import os

import requests
import yaml


class PcapComplianceClient:
    def __init__(self, pcap_file=None, config_path="ebu_list.yaml", proxies={'http': "", "https": "", "ftp": ""}):
        """
        Initialize the client with optional PCAP file and config path.
        Loads EBU server IP and credentials from the YAML config.
        """
        self.pcap_file = pcap_file
        self.token = None
        self.ebu_ip = None
        self.user = None
        self.password = None
        self.pcap_id = None
        self.report_dir = None
        self.proxies = proxies
        self.session = requests.Session()
        self.session.trust_env = False  # Do not use system proxy settings

        # Load EBU IP and credentials from YAML config
        if config_path:
            with open(config_path, "r") as f:
                config = yaml.safe_load(f)
            instance = config["instances"]
            self.ebu_ip = instance.get("name", "")
            self.user = instance.get("username", "")
            self.password = instance.get("password", "")

    def authenticate(self):
        """
        Authenticate with the EBU server and store the access token.
        """
        url = f"http://{self.ebu_ip}/auth/login"
        headers = {'Content-Type': 'application/json'}
        data = {"username": self.user, "password": self.password}
        response = self.session.post(url, headers=headers, json=data, verify=False, proxies=self.proxies)
        response.raise_for_status()
        self.token = response.json().get('content', {}).get('token')
        if not self.token:
            raise Exception("Authentication failed: No token received.")

    def upload_pcap(self):
        """
        Upload the PCAP file to the EBU server and store the returned UUID.
        Returns the UUID of the uploaded PCAP.
        """
        url = f"http://{self.ebu_ip}/api/pcap"
        headers = {'Authorization': f'Bearer {self.token}'}
        if self.pcap_file:
            with open(self.pcap_file, 'rb') as f:
                files = {'pcap': (os.path.basename(self.pcap_file), f, 'application/vnd.tcpdump.pcap')}
                response = self.session.put(url, headers=headers, files=files, verify=False, proxies=self.proxies)
            response.raise_for_status()
            self.pcap_id = response.json().get('uuid')
        if not self.pcap_id:
            raise Exception("Upload failed: No UUID received.")
        print(f"Extracted UUID: >>>{self.pcap_id}<<<")
        return self.pcap_id

    def download_report(self, test_name):
        """
        Download the compliance report for the uploaded PCAP file.
        Saves the report as a JSON file in a timestamped directory.
        Returns the path to the saved report.
        """
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        self.report_dir = os.path.join("reports", test_name, timestamp)
        os.makedirs(self.report_dir, exist_ok=True)
        url = f"http://{self.ebu_ip}/api/pcap/{self.pcap_id}/report?type=json"
        headers = {'Authorization': f'Bearer {self.token}'}
        response = self.session.get(url, headers=headers, verify=False, proxies=self.proxies)
        response.raise_for_status()
        report_path = os.path.join(self.report_dir, f"{self.pcap_id}.json")
        with open(report_path, "w") as f:
            json.dump(response.json(), f, indent=2)
        return report_path

    def check_compliance(self, report_path):
        """
        Check the compliance result from the downloaded report.
        Prints the result, writes PASSED/FAILED files, and lists error IDs if failed.
        Returns True if compliant, False otherwise.
        """
        with open(report_path, "r") as f:
            report = json.load(f)
        is_compliant = report.get("not_compliant_streams", 1) == 0
        result_file = "PASSED" if is_compliant else "FAILED"
        print(f"Result: {result_file}")
        with open(os.path.join(self.report_dir, result_file), "w") as f_result:
            if is_compliant:
                f_result.write("")
            else:
                error_ids = []
                for err in report.get("summary", {}).get("error_list", []):
                    error_id = err.get("value", {}).get("id")
                    print(error_id)
                    error_ids.append(str(error_id))
                for error_id in error_ids:
                    f_result.write(f"{error_id}\n")
        print(f"Json file saved: file://{os.path.abspath(report_path)}")
        return is_compliant

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
        headers = {'Authorization': f'Bearer {self.token}'}
        response = self.session.delete(url, headers=headers, verify=False, proxies=self.proxies)
        if response.status_code == 200:
            print(f"PCAP {pcap_id} deleted successfully from EBU server.")
        else:
            print(f"Failed to delete PCAP {pcap_id}: {response.status_code} {response.text}")
            response.raise_for_status()
