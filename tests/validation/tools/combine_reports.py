#!/usr/bin/env python3
"""Combine individual pytest HTML reports into a single Excel summary."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, Iterable, Tuple

import pandas as pd
from bs4 import BeautifulSoup

STATUS_CLASSES = {
    "passed",
    "failed",
    "skipped",
    "xfailed",
    "xpassed",
    "error",
    "warning",
    "notrun",
    "rerun",
}

STATUS_LABELS = {
    "passed": "PASSED",
    "failed": "FAILED",
    "skipped": "SKIPPED",
    "xfailed": "XFAILED",
    "xpassed": "XPASSED",
    "error": "ERROR",
    "warning": "WARNING",
    "notrun": "NOT RUN",
    "rerun": "RERUN",
    "unknown": "UNKNOWN",
}

DEFAULT_STATUS = "NOT FOUND"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Combine pytest HTML reports generated for different NICs into an Excel file."
    )
    parser.add_argument(
        "--report",
        action="append",
        required=True,
        metavar="NIC=PATH",
        help="Mapping between a NIC label and the HTML report path (e.g. '--report E810=reports/e810/report.html').",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("combined_report.xlsx"),
        help="Destination Excel file path (default: combined_report.xlsx).",
    )
    return parser.parse_args()


def parse_report_options(values: Iterable[str]) -> Dict[str, Path]:
    mapping: Dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"Invalid --report option '{value}'. Expected format NIC=PATH.")
        nic, raw_path = value.split("=", 1)
        nic = nic.strip()
        if not nic:
            raise ValueError(f"Invalid NIC label in option '{value}'.")
        path = Path(raw_path).expanduser().resolve()
        mapping[nic] = path
    return mapping


def read_html(path: Path) -> BeautifulSoup:
    html_text = path.read_text(encoding="utf-8", errors="ignore")
    return BeautifulSoup(html_text, "html.parser")


def parse_report(path: Path) -> Dict[Tuple[str, str], str]:
    soup = read_html(path)
    results: Dict[Tuple[str, str], str] = {}

    for file_details in soup.select("details.file"):
        file_path_element = file_details.select_one(".fspath")
        if not file_path_element:
            continue
        file_path = file_path_element.get_text(strip=True)

        for test_details in file_details.select("details.test"):
            status_token = next(
                (cls for cls in test_details.get("class", []) if cls in STATUS_CLASSES),
                "unknown",
            )
            test_name_element = test_details.select_one(".test-name")
            if test_name_element:
                test_name = test_name_element.get_text(separator=" ", strip=True)
            else:
                title_element = test_details.select_one(".title")
                test_name = (
                    title_element.get_text(separator=" ", strip=True)
                    if title_element
                    else "UNKNOWN"
                )

            results[(file_path, test_name)] = STATUS_LABELS.get(
                status_token, STATUS_LABELS["unknown"]
            )

    return results


def build_dataframe(
    keys: Iterable[Tuple[str, str]], data: Dict[str, Dict[Tuple[str, str], str]]
) -> pd.DataFrame:
    ordered_tests = sorted(set(keys), key=lambda item: (item[0], item[1]))
    rows = []

    for test_file, test_case in ordered_tests:
        row = {
            "Test File": test_file,
            "Test Case": test_case,
        }
        for nic, mapping in data.items():
            row[nic] = mapping.get((test_file, test_case), DEFAULT_STATUS)
        rows.append(row)

    return pd.DataFrame(rows)


def main() -> None:
    args = parse_args()
    reports = parse_report_options(args.report)

    parsed_data: Dict[str, Dict[Tuple[str, str], str]] = {}
    all_keys = set()

    for nic_name, report_path in reports.items():
        if not report_path.exists():
            raise FileNotFoundError(f"Report not found for {nic_name}: {report_path}")
        parsed = parse_report(report_path)
        parsed_data[nic_name] = parsed
        all_keys.update(parsed.keys())
        print(f"Parsed {len(parsed)} tests for {nic_name} from {report_path}")

    if not all_keys:
        raise RuntimeError("No tests discovered across provided reports.")

    df = build_dataframe(all_keys, parsed_data)
    output_path = args.output
    output_path.parent.mkdir(parents=True, exist_ok=True)
    df.to_excel(output_path, index=False)
    print(f"Combined report written to {output_path}")


if __name__ == "__main__":
    main()
