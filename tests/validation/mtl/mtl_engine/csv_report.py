# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
import csv

report = []


def csv_add_test(
    test_case: str, commands: str, result: str, issue: str, result_note: str = None
):
    report.append(
        {
            "Test case": test_case,
            "Commands": commands,
            "Result": result,
            "Issue": issue,
            "Result note": result_note,
        }
    )


def csv_write_report(filename: str):
    with open(filename, "w", newline="") as csvfile:
        fieldnames = [
            "ID",
            "Test case",
            "Commands",
            "Status",
            "Result",
            "Issue",
            "Result note",
        ]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        for nr, r in enumerate(report):
            r["ID"] = nr + 1
            r["Status"] = "Executed"
            writer.writerow(r)
