# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
import csv

report = []


def csv_add_test(test_case: str, commands: str, result: str, issue: str, result_note: str = None):
    report.append(
        {"Test case": test_case, "Commands": commands, "Result": result, "Issue": issue, "Result note": result_note}
    )


def csv_write_report(filename: str):
    with open(filename, "w", newline="") as csvfile:
        fieldnames = ["ID", "Test case", "Commands", "Status", "Result", "Issue", "Result note"]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        for nr, r in enumerate(report):
            r["ID"] = nr + 1
            r["Status"] = "Executed"
            writer.writerow(r)
