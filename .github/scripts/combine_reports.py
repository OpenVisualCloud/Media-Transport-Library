#!/usr/bin/env python3
"""Combine individual pytest HTML reports into a single Excel summary."""

from __future__ import annotations

import argparse
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

import pandas as pd
from bs4 import BeautifulSoup

# Constants
STATUS_CLASSES = {
    "passed",
    "failed",
    "skipped",
    "xpassed",
    "xfailed",
    "error",
    "warning",
    "notrun",
    "rerun",
}

STATUS_LABELS = {
    "passed": "PASSED",
    "failed": "FAILED",
    "skipped": "SKIPPED",
    "xpassed": "XPASSED",
    "xfailed": "XFAILED",
    "error": "ERROR",
    "warning": "WARNING",
    "notrun": "NOT RUN",
    "rerun": "RERUN",
    "unknown": "UNKNOWN",
}

DEFAULT_STATUS = "NOT FOUND"
NIC_TYPES = ["E810", "E810-Dell", "E830"]
METRICS = [
    "Passed",
    "Failed",
    "Skipped",
    "Error",
    "XPassed",
    "XFailed",
    "Other",
    "Total",
]
NO_DATA_MESSAGE = "No test data discovered across provided reports"
OTHER_STATUS_LABELS = {
    "UNKNOWN",
    "NOT RUN",
    "WARNING",
    "RERUN",
    "NOT FOUND",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Combine pytest HTML reports generated for different NICs into an Excel file.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Auto-discover reports in current directory
  %(prog)s

  # Specify directory with reports
  %(prog)s --directory ./reports

  # Generate timestamped output
  %(prog)s --directory ./reports --timestamp

  # GitHub Actions usage (auto-detects artifacts)
  %(prog)s --directory downloaded-artifacts --output results/combined.xlsx
        """,
    )
    parser.add_argument(
        "--directory",
        type=Path,
        default=None,
        help=(
            "Directory containing HTML reports with pattern "
            "nightly-test-report-{nic}-{category}.html (default: current directory)"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Destination Excel file path (default: combined_report.xlsx or timestamped if --timestamp used)",
    )
    parser.add_argument(
        "--timestamp",
        action="store_true",
        help="Add timestamp to output filename (format: MTL_test_report_YYYY-MM-DD_HH-MM.xlsx)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress progress messages (only show warnings and errors)",
    )
    parser.add_argument(
        "--print-path",
        action="store_true",
        help="Print the absolute path of the generated report to stdout",
    )
    return parser.parse_args()


def log_message(message: str, quiet: bool = False, is_error: bool = False) -> None:
    """Print message to appropriate stream based on quiet mode."""
    if is_error:
        print(message, file=sys.stderr)
    elif not quiet:
        print(message)


def get_output_path(args: argparse.Namespace) -> Path:
    """Determine the output file path based on arguments."""
    if args.output:
        return args.output.expanduser().resolve()

    if args.timestamp:
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M")
        filename = f"MTL_test_report_{timestamp}.xlsx"
    else:
        filename = "combined_report.xlsx"

    # If directory mode, place output in the same directory
    if args.directory:
        directory = args.directory.expanduser().resolve()
        return directory / filename

    # Otherwise use current directory
    return Path.cwd() / filename


def discover_reports(directory: Path) -> Dict[str, Dict[str, Path]]:
    """
    Auto-discover reports in directory with pattern: nightly-test-report-{nic}-{category}.html
    Returns: Dict[category, Dict[nic_type, Path]]
    """
    pattern = re.compile(
        r"nightly-test-report-(e810-dell|e810|e830)-(.+)\.html", re.IGNORECASE
    )
    reports_by_category: Dict[str, Dict[str, Path]] = {}

    nic_mapping = {
        "e810": "E810",
        "e810-dell": "E810-Dell",
        "e830": "E830",
    }

    for html_file in directory.glob("nightly-test-report-*.html"):
        match = pattern.match(html_file.name)
        if match:
            nic_raw = match.group(1).lower()
            category = match.group(2)

            nic_label = nic_mapping.get(nic_raw)
            if not nic_label:
                continue

            if category not in reports_by_category:
                reports_by_category[category] = {}
            reports_by_category[category][nic_label] = html_file

    return reports_by_category


def read_html(path: Path) -> BeautifulSoup:
    html_text = path.read_text(encoding="utf-8", errors="ignore")
    return BeautifulSoup(html_text, "html.parser")


def get_test_name(test_details) -> str:
    """Extract test name from test details element."""
    test_name_element = test_details.select_one(".test-name")
    if test_name_element:
        return test_name_element.get_text(separator=" ", strip=True)

    title_element = test_details.select_one(".title")
    return (
        title_element.get_text(separator=" ", strip=True)
        if title_element
        else "UNKNOWN"
    )


def parse_report(path: Path) -> Dict[Tuple[str, str], str]:
    """Parse HTML report and extract test results."""
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
            test_name = get_test_name(test_details)
            results[(file_path, test_name)] = STATUS_LABELS.get(
                status_token, STATUS_LABELS["unknown"]
            )

    return results


def build_dataframe(
    keys: Iterable[Tuple[str, str]],
    data: Dict[str, Dict[Tuple[str, str], str]],
    category: str = None,
) -> pd.DataFrame:
    """Build a DataFrame from test results."""
    ordered_tests = sorted(set(keys), key=lambda item: (item[0], item[1]))
    nic_columns = list(data.keys())

    rows = []
    for test_file, test_case in ordered_tests:
        row = {}
        if category:
            row["Category"] = category
        row["Test File"] = test_file
        row["Test Case"] = test_case

        seen_statuses = set()
        for nic in nic_columns:
            row[nic] = data[nic].get((test_file, test_case), DEFAULT_STATUS)
            seen_statuses.add(row[nic])

        row["Comments"] = "Inconsistent across NICs" if len(seen_statuses) > 1 else ""
        rows.append(row)

    columns = (
        ["Category", "Test File", "Test Case", *nic_columns, "Comments"]
        if category
        else ["Test File", "Test Case", *nic_columns, "Comments"]
    )
    return pd.DataFrame(rows, columns=columns)


def get_nic_columns(df: pd.DataFrame) -> List[str]:
    """Extract NIC column names from a DataFrame."""
    excluded_cols = {"Category", "Test File", "Test Case", "Comments"}
    return [col for col in df.columns if col not in excluded_cols]


def calculate_status_counts(df: pd.DataFrame, nic: str) -> Dict[str, int]:
    """Calculate status counts for a specific NIC column."""
    status_counts = df[nic].value_counts()
    counts = {
        "Passed": status_counts.get("PASSED", 0),
        "Failed": status_counts.get("FAILED", 0),
        "Skipped": status_counts.get("SKIPPED", 0),
        "Error": status_counts.get("ERROR", 0),
        "XPassed": status_counts.get("XPASSED", 0),
        "XFailed": status_counts.get("XFAILED", 0),
    }

    counts["Other"] = sum(status_counts.get(label, 0) for label in OTHER_STATUS_LABELS)
    counts["Total"] = sum(counts.values())
    return counts


def build_summary(all_dataframes: List[pd.DataFrame]) -> pd.DataFrame:
    """Build a summary table with statistics per category and NIC."""
    summary_rows = []

    for df in all_dataframes:
        if df.empty:
            continue

        category = df["Category"].iloc[0] if "Category" in df.columns else "Unknown"
        nic_columns = get_nic_columns(df)

        row = {"Category": category}
        for nic in nic_columns:
            counts = calculate_status_counts(df, nic)
            for metric, value in counts.items():
                row[f"{nic}_{metric}"] = value

        summary_rows.append(row)

    # Add totals row
    if summary_rows:
        total_row = {"Category": "TOTAL"}
        for nic in NIC_TYPES:
            for metric in METRICS:
                col_name = f"{nic}_{metric}"
                total_row[col_name] = sum(row.get(col_name, 0) for row in summary_rows)
        summary_rows.append(total_row)

    return pd.DataFrame(summary_rows)


def write_summary_headers(worksheet, nic_types: List[str], metrics: List[str]) -> None:
    """Write multi-level headers for the summary section."""
    # Row 0: NIC grouping
    worksheet.cell(row=1, column=1, value="Category")
    col = 2
    for nic in nic_types:
        worksheet.cell(row=1, column=col, value=nic)
        worksheet.merge_cells(
            start_row=1, start_column=col, end_row=1, end_column=col + len(metrics) - 1
        )
        col += len(metrics)

    # Row 1: Metric headers
    worksheet.cell(row=2, column=1, value="Category")
    col = 2
    for nic in nic_types:
        for metric in metrics:
            worksheet.cell(row=2, column=col, value=metric)
            col += 1


def write_combined_report(
    output_path: Path, summary_df: pd.DataFrame, combined_df: pd.DataFrame
) -> None:
    """Write summary and detailed results to Excel with proper formatting."""
    with pd.ExcelWriter(output_path, engine="openpyxl") as writer:
        # Write summary without header (we'll add custom headers)
        # Leave two rows for the custom multi-level headers before data
        summary_df.to_excel(
            writer, sheet_name="Sheet1", index=False, startrow=2, header=False
        )

        worksheet = writer.sheets["Sheet1"]
        write_summary_headers(worksheet, NIC_TYPES, METRICS)

        # Calculate where detailed results start (2 header rows + summary rows + 1 blank row)
        separator_row = len(summary_df) + 3
        combined_df.to_excel(
            writer, sheet_name="Sheet1", index=False, startrow=separator_row
        )


def build_placeholder_dataframe(message: str) -> pd.DataFrame:
    """Return a DataFrame containing a single placeholder row."""
    placeholder_row = {
        "Category": "-",
        "Test File": "-",
        "Test Case": "-",
        "Comments": message,
    }
    for nic in NIC_TYPES:
        placeholder_row[nic] = DEFAULT_STATUS

    columns = ["Category", "Test File", "Test Case", *NIC_TYPES, "Comments"]
    return pd.DataFrame([placeholder_row], columns=columns)


def process_category(
    category: str, nic_reports: Dict[str, Path], quiet: bool = False
) -> pd.DataFrame:
    """Process reports for a single category and return combined DataFrame."""
    log_message(f"\nProcessing category: {category}", quiet)

    parsed_data: Dict[str, Dict[Tuple[str, str], str]] = {}
    all_keys = set()

    for nic_name in NIC_TYPES:
        if nic_name in nic_reports:
            report_path = nic_reports[nic_name]
            parsed = parse_report(report_path)
            parsed_data[nic_name] = parsed
            all_keys.update(parsed.keys())
            log_message(f"  Parsed {len(parsed)} tests for {nic_name}", quiet)

    if not all_keys:
        log_message(f"  Warning: No tests found for {category}", quiet, is_error=True)
        return pd.DataFrame()

    return build_dataframe(all_keys, parsed_data, category=category)


def process_directory_mode(directory: Path, output: Path, quiet: bool = False) -> None:
    """Process reports in directory discovery mode."""
    directory = directory.expanduser().resolve()
    if not directory.exists():
        raise FileNotFoundError(f"Directory not found: {directory}")

    reports_by_category = discover_reports(directory)
    if not reports_by_category:
        log_message(f"Warning: No reports found in {directory}", quiet, is_error=True)
        reports_by_category = {}

    if reports_by_category:
        log_message(f"Discovered {len(reports_by_category)} test categories", quiet)
    else:
        log_message("No categories detected; creating placeholder report", quiet)

    # Process all categories
    all_dataframes = []
    for category in sorted(reports_by_category.keys()):
        df = process_category(category, reports_by_category[category], quiet)
        if not df.empty:
            all_dataframes.append(df)

    if not all_dataframes:
        log_message("No data to write", quiet, is_error=True)
        all_dataframes = [build_placeholder_dataframe(NO_DATA_MESSAGE)]

    # Build summary and write report
    summary_df = build_summary(all_dataframes)
    combined_df = pd.concat(all_dataframes, ignore_index=True)
    write_combined_report(output, summary_df, combined_df)

    log_message(
        f"\nCombined report written to {output} with {len(combined_df)} total tests",
        quiet,
    )


def main() -> None:
    """Main entry point."""
    args = parse_args()

    # Determine output path
    output_path = get_output_path(args)

    # Default to current directory if not specified
    if not args.directory:
        args.directory = Path.cwd()
        log_message(
            f"No directory specified, using current directory: {args.directory}",
            args.quiet,
        )

    try:
        process_directory_mode(args.directory, output_path, args.quiet)

        # Print absolute path if requested (useful for CI/CD)
        if args.print_path and output_path.exists():
            print(output_path.resolve())

    except Exception as e:
        log_message(f"Error: {e}", args.quiet, is_error=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
