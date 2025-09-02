import os
import sys

from csv_report import TestCSVReport
from pcap_compliance import PcapComplianceClient

# Usage:
# python3 check_compliance.py <uuid> <test_name> [csv_path]
# <uuid>      : UUID of the uploaded PCAP file (from upload step)
# <test_name> : Name for the test (used for report directory)
# [csv_path]  : (Optional) Path to CSV file for appending compliance results and path to logs

if len(sys.argv) not in (3, 4):
    print("Usage: python3 check_compliance.py <uuid> <test_name> [csv_path]")
    sys.exit(1)

uuid = sys.argv[1]
test_name = sys.argv[2]
csv_path = sys.argv[3] if len(sys.argv) == 4 else None

# Step 1: Run compliance check and generate report/logs using PcapComplianceClient
checker = PcapComplianceClient()
checker.authenticate()
checker.pcap_id = uuid
report_path = checker.download_report(test_name)
is_compliant = checker.check_compliance(report_path)
print(f"Report and logs saved in: {os.path.abspath(checker.report_dir)}")

# Step 2 (optional): If csv_path is provided, append compliance result to CSV
if csv_path:
    report = TestCSVReport(csv_path)
    compliance_result = "PASSED" if is_compliant else "FAILED"
    report.add_result(test_name, compliance_result, checker.report_dir)
    print(f"Compliance result added to CSV: {csv_path}")

# Step 3 Delete the PCAP analysis from the EBU server
checker.delete_pcap(uuid)
