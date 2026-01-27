#!/usr/bin/env python3
"""Combine nightly gtest XML artifacts into a single Excel summary report."""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple

import pandas as pd

NIC_PATTERN = re.compile(r"nightly-gtest-report-([a-z0-9-]+)", re.IGNORECASE)
NIC_LABELS = {
    "e810": "E810",
    "e810-dell": "E810-Dell",
    "e830": "E830",
}
NIC_DISPLAY_ORDER = ["E810", "E810-Dell", "E830"]
STATUS_PASSED = "PASSED"
STATUS_FAILED = "FAILED"
STATUS_SKIPPED = "SKIPPED"
STATUS_UNKNOWN = "UNKNOWN"
DEFAULT_STATUS = "NOT RUN"
STATUS_PRIORITY = {
    STATUS_FAILED: 4,
    STATUS_SKIPPED: 3,
    STATUS_PASSED: 2,
    STATUS_UNKNOWN: 1,
    DEFAULT_STATUS: 0,
}
SUMMARY_METRICS = ["Passed", "Failed", "Skipped", "Missing", "Other", "Total"]
NO_DATA_MESSAGE = "No gtest XML files discovered across provided artifacts"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Combine nightly gtest log artifacts into an Excel summary report.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Auto-discover logs in the current directory
  %(prog)s

  # Specify artifact directory
  %(prog)s --directory ./gtest-reports

  # Emit timestamped filename and print absolute path
  %(prog)s --directory ./gtest-reports --timestamp --print-path
        """,
    )
    parser.add_argument(
        "--directory",
        type=Path,
        default=None,
        help="Directory containing nightly-gtest-report-* log artifacts (default: current directory)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Destination Excel file path (default: gtest_combined_report.xlsx or timestamped variant)",
    )
    parser.add_argument(
        "--timestamp",
        action="store_true",
        help="Add timestamp to output filename (MTL_gtest_report_YYYY-MM-DD_HH-MM.xlsx)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress progress messages (warnings and errors still shown)",
    )
    parser.add_argument(
        "--print-path",
        action="store_true",
        help="Print the absolute path of the generated report to stdout",
    )
    return parser.parse_args()


def log_message(message: str, quiet: bool = False, is_error: bool = False) -> None:
    if is_error:
        print(message, file=sys.stderr)
    elif not quiet:
        print(message)


def get_output_path(args: argparse.Namespace) -> Path:
    if args.output:
        return args.output.expanduser().resolve()

    if args.timestamp:
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M")
        filename = f"MTL_gtest_report_{timestamp}.xlsx"
    else:
        filename = "gtest_combined_report.xlsx"

    base_dir = args.directory.expanduser().resolve() if args.directory else Path.cwd()
    return base_dir / filename


def normalize_nic_name(raw: str) -> str:
    return NIC_LABELS.get(raw.lower(), raw.upper())


def extract_nic_label(path: Path) -> str | None:
    candidates = [path.stem, path.name]
    if path.parent and path.parent != path:
        candidates.append(path.parent.name)
        candidates.append(path.parent.stem)
    if path.parent.parent and path.parent.parent != path.parent:
        candidates.append(path.parent.parent.name)
    for candidate in candidates:
        match = NIC_PATTERN.search(candidate)
        if match:
            return normalize_nic_name(match.group(1))
    return None


def discover_xml_files(directory: Path) -> Dict[str, List[Path]]:
    xml_files: Dict[str, List[Path]] = {}
    for xml_path in directory.rglob("nightly-gtest-report-*.xml"):
        nic_label = extract_nic_label(xml_path)
        if not nic_label:
            continue
        xml_files.setdefault(nic_label, []).append(xml_path)
    return xml_files


def record_case_result(
    results: Dict[Tuple[str, str], Dict[str, object]],
    key: Tuple[str, str],
    status: str,
    detail: str,
) -> None:
    priority = STATUS_PRIORITY.get(status, 0)
    existing = results.get(key)
    if not existing or priority >= existing["priority"]:
        results[key] = {
            "status": status,
            "details": detail,
            "priority": priority,
        }


def _extract_detail(node: ET.Element | None) -> str:
    if node is None:
        return ""
    message = node.attrib.get("message", "").strip()
    text = (node.text or "").strip()
    if message and text:
        return f"{message}: {text}"
    return message or text


def parse_gtest_xml(path: Path) -> Dict[Tuple[str, str], Dict[str, object]]:
    try:
        tree = ET.parse(path)
    except ET.ParseError as exc:  # pragma: no cover - defensive guard
        raise RuntimeError(f"Failed to parse XML {path}: {exc}") from exc
    except OSError as exc:
        raise RuntimeError(f"Failed to read XML {path}: {exc}") from exc

    results: Dict[Tuple[str, str], Dict[str, object]] = {}
    for case in tree.iterfind(".//testcase"):
        suite = (
            case.attrib.get("classname") or case.attrib.get("name", "").split(".")[0]
        )
        test_name = case.attrib.get("name") or "UNKNOWN"
        key = (suite or "UNKNOWN_SUITE", test_name)

        failure = case.find("failure")
        error = case.find("error")
        skipped = case.find("skipped")
        status_attr = (
            case.attrib.get("status") or case.attrib.get("result") or ""
        ).lower()

        if failure is not None:
            status = STATUS_FAILED
            detail = _extract_detail(failure)
        elif error is not None:
            status = STATUS_FAILED
            detail = _extract_detail(error)
        elif skipped is not None:
            status = STATUS_SKIPPED
            detail = _extract_detail(skipped)
        elif status_attr == "notrun":
            status = DEFAULT_STATUS
            detail = ""
        else:
            status = STATUS_PASSED
            detail = ""

        record_case_result(results, key, status, detail)

    return results


def determine_nic_columns(nic_results: Dict[str, Dict[str, object]]) -> List[str]:
    ordered = [nic for nic in NIC_DISPLAY_ORDER if nic in nic_results]
    extras = sorted(nic for nic in nic_results if nic not in ordered)
    return ordered + extras


def build_detailed_dataframe(
    nic_results: Dict[str, Dict[Tuple[str, str], Dict[str, object]]],
    nic_columns: List[str],
) -> pd.DataFrame:
    if not nic_columns:
        return pd.DataFrame()

    test_keys = sorted(
        {key for results in nic_results.values() for key in results},
        key=lambda item: (item[0], item[1]),
    )
    if not test_keys:
        return pd.DataFrame(
            columns=["Test Suite", "Test Case", *nic_columns, "Comments"]
        )

    rows = []
    for suite, test_case in test_keys:
        row = {"Test Suite": suite, "Test Case": test_case}
        statuses = set()
        comments: List[str] = []
        for nic in nic_columns:
            nic_data = nic_results.get(nic, {})
            entry = nic_data.get((suite, test_case))
            if entry:
                status = entry["status"]
                detail = entry.get("details", "")
            else:
                status = DEFAULT_STATUS
                detail = ""
            row[nic] = status
            statuses.add(status)
            if detail and status != STATUS_PASSED:
                comments.append(f"{nic}: {detail}")
        if len(statuses) > 1:
            comments.insert(0, "Inconsistent across NICs")
        row["Comments"] = " | ".join(comments)
        rows.append(row)

    columns = ["Test Suite", "Test Case", *nic_columns, "Comments"]
    return pd.DataFrame(rows, columns=columns).sort_values(
        ["Test Suite", "Test Case"], ignore_index=True
    )


def build_placeholder_dataframe(message: str, nic_columns: List[str]) -> pd.DataFrame:
    if not nic_columns:
        nic_columns = NIC_DISPLAY_ORDER.copy()
    placeholder = {"Test Suite": "-", "Test Case": "-", "Comments": message}
    for nic in nic_columns:
        placeholder[nic] = DEFAULT_STATUS
    columns = ["Test Suite", "Test Case", *nic_columns, "Comments"]
    return pd.DataFrame([placeholder], columns=columns)


def calculate_summary_counts(column: pd.Series) -> Dict[str, int]:
    counts = column.value_counts(dropna=False)
    summary = {
        "Passed": int(counts.get(STATUS_PASSED, 0)),
        "Failed": int(counts.get(STATUS_FAILED, 0)),
        "Skipped": int(counts.get(STATUS_SKIPPED, 0)),
        "Missing": int(counts.get(DEFAULT_STATUS, 0)),
    }
    known_total = sum(summary.values())
    summary["Other"] = int(counts.sum()) - known_total
    summary["Total"] = int(counts.sum())
    return summary


def build_summary_dataframe(
    detailed_df: pd.DataFrame, nic_columns: List[str]
) -> pd.DataFrame:
    rows = []
    for nic in nic_columns:
        if nic not in detailed_df.columns:
            summary = {metric: 0 for metric in SUMMARY_METRICS}
        else:
            summary = calculate_summary_counts(detailed_df[nic])
        row = {"NIC": nic}
        row.update(summary)
        rows.append(row)

    if rows:
        total_row = {"NIC": "TOTAL"}
        for metric in SUMMARY_METRICS:
            total_row[metric] = sum(row[metric] for row in rows)
        rows.append(total_row)

    return pd.DataFrame(rows, columns=["NIC", *SUMMARY_METRICS])


def write_excel_report(
    output_path: Path, summary_df: pd.DataFrame, detailed_df: pd.DataFrame
) -> None:
    with pd.ExcelWriter(output_path, engine="openpyxl") as writer:
        summary_df.to_excel(writer, sheet_name="Summary", index=False)
        detailed_df.to_excel(writer, sheet_name="Details", index=False)


def process_directory(directory: Path, output: Path, quiet: bool = False) -> None:
    directory = directory.expanduser().resolve()
    if not directory.exists():
        raise FileNotFoundError(f"Directory not found: {directory}")

    xml_files = discover_xml_files(directory)
    if not xml_files:
        log_message(f"Warning: No XML files found in {directory}", quiet, is_error=True)

    nic_results: Dict[str, Dict[Tuple[str, str], Dict[str, object]]] = {}
    for nic, files in sorted(xml_files.items()):
        combined: Dict[Tuple[str, str], Dict[str, object]] = {}
        for xml_path in sorted(files):
            parsed = parse_gtest_xml(xml_path)
            for key, data in parsed.items():
                existing = combined.get(key)
                if not existing or data["priority"] >= existing["priority"]:
                    combined[key] = data
        nic_results[nic] = combined
        log_message(
            f"Parsed {len(combined)} unique tests for {nic} ({len(files)} XML files)",
            quiet,
        )

    nic_columns = determine_nic_columns(nic_results)
    if not nic_columns:
        nic_columns = NIC_DISPLAY_ORDER.copy()

    detailed_df = build_detailed_dataframe(nic_results, nic_columns)
    if detailed_df.empty:
        detailed_df = build_placeholder_dataframe(NO_DATA_MESSAGE, nic_columns)

    summary_df = build_summary_dataframe(detailed_df, nic_columns)
    write_excel_report(output, summary_df, detailed_df)

    log_message(
        f"Combined gtest report written to {output} with {len(detailed_df)} entries",
        quiet,
    )


def main() -> None:
    args = parse_args()

    if not args.directory:
        args.directory = Path.cwd()
        log_message(
            f"No directory specified, using current directory: {args.directory}",
            args.quiet,
        )

    output_path = get_output_path(args)

    try:
        process_directory(args.directory, output_path, args.quiet)
    except Exception as exc:  # pylint: disable=broad-except
        log_message(f"Error: {exc}", args.quiet, is_error=True)
        sys.exit(1)

    if args.print_path and output_path.exists():
        print(output_path.resolve())


if __name__ == "__main__":
    main()
