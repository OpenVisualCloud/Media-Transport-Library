# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""HTML report generation for MTL nightly test reports."""

import re
from collections import defaultdict
from datetime import datetime
from itertools import groupby
from math import pi

# Column definitions for test result tables: (header_label, data_key)
_PYTEST_COLUMNS = [
    ("NIC", "nic"),
    ("Category", "category"),
    ("Runner", "runner"),
    ("Passed", "passed"),
    ("Failed", "failed"),
    ("Skipped", "skipped"),
    ("Error", "error"),
    ("XPassed", "xpassed"),
    ("XFailed", "xfailed"),
    ("Total", "total"),
]

_GTEST_COLUMNS = [
    ("NIC", "nic"),
    ("Category", "category"),
    ("Runner", "runner"),
    ("Passed", "passed"),
    ("Failed", "failed"),
    ("Skipped", "skipped"),
    ("Total", "total"),
]

# Regression detail sections: (data_key, label, subtitle, show_baseline, css_mod)
_REGRESSION_SECTIONS = [
    ("regressions", "Regressions", "previously passed, now failing", True, "danger"),
    ("new_failures", "New Failures", "not present in baseline", False, "warning"),
    ("fixes", "Fixes", "previously failing, now passing", True, "success"),
]

# SVG donut chart colors
_CHART_COLORS = {
    "passed": "#34a853",
    "failed": "#ea4335",
    "error": "#8c0c00",
    "xfailed": "#1a73e8",
    "skipped": "#f9ab00",
}

# Application display order
_APP_ORDER = ["rxtxapp", "ffmpeg", "gstreamer"]

# Statuses that carry displayable log output
_LOG_STATUSES = ("FAILED", "ERROR", "SKIPPED")

_APPLICATION_RE = re.compile(r"\|application\s*=\s*(\w+)\|")


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def generate_html_report(
    pytest_data,
    gtest_data,
    output_file,
    system_info_list=None,
    test_metadata=None,
    regression_data=None,
):
    """Create HTML report combining pytest and gtest results."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S UTC")
    stats = _calculate_statistics(pytest_data, gtest_data)

    html = _get_html_template().format(
        timestamp=timestamp,
        test_run_info=_generate_test_run_info(test_metadata),
        system_info_section=(
            _generate_system_info(system_info_list) if system_info_list else ""
        ),
        overall_summary=_generate_summary_cards(
            "Overall Summary", stats["combined"], "overall"
        ),
        pytest_summary=(
            _generate_summary_cards("Pytest Summary", stats["pytest"], "pytest")
            if pytest_data
            else ""
        ),
        gtest_summary=(
            _generate_summary_cards("GTest Summary", stats["gtest"], "gtest")
            if gtest_data
            else ""
        ),
        regression_section=(
            _generate_regression_section(regression_data) if regression_data else ""
        ),
        pytest_table=_generate_pytest_table(pytest_data) if pytest_data else "",
        gtest_table=_generate_gtest_table(gtest_data) if gtest_data else "",
    )

    with open(output_file, "w") as f:
        f.write(html)

    print(f"HTML report saved to: {output_file}")


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


def _calculate_statistics(pytest_data, gtest_data):
    """Calculate test statistics from data."""

    def _suite_stats(data):
        if not data:
            return {
                "total": 0,
                "passed": 0,
                "failed": 0,
                "skipped": 0,
                "error": 0,
                "xfailed": 0,
                "pass_rate": 0,
            }
        passed = sum(d.get("passed", 0) for d in data)
        failed = sum(d.get("failed", 0) for d in data)
        error = sum(d.get("error", 0) for d in data)
        xfailed = sum(d.get("xfailed", 0) for d in data)
        denominator = passed + failed + error + xfailed
        return {
            "total": sum(d.get("total", 0) for d in data),
            "passed": passed,
            "failed": failed,
            "skipped": sum(d.get("skipped", 0) for d in data),
            "error": error,
            "xfailed": xfailed,
            "pass_rate": (
                (passed + xfailed) / denominator * 100 if denominator > 0 else 0
            ),
        }

    p = _suite_stats(pytest_data)
    g = _suite_stats(gtest_data)
    cp = p["passed"] + g["passed"]
    cf = p["failed"] + g["failed"]
    ce = p["error"] + g["error"]
    cxf = p["xfailed"] + g["xfailed"]
    denominator = cp + cf + ce + cxf
    return {
        "pytest": p,
        "gtest": g,
        "combined": {
            "total": p["total"] + g["total"],
            "passed": cp,
            "failed": cf,
            "skipped": p["skipped"] + g["skipped"],
            "error": ce,
            "xfailed": cxf,
            "pass_rate": (cp + cxf) / denominator * 100 if denominator > 0 else 0,
        },
    }


# ---------------------------------------------------------------------------
# Summary cards
# ---------------------------------------------------------------------------


def _generate_summary_cards(title, stats, section_prefix=""):
    """Generate a summary card grid. Non-zero failed/error/skipped cards link to details."""
    rate = stats["pass_rate"]
    rate_cls = (
        "card-success"
        if rate >= 95
        else "card-warning" if rate >= 80 else "card-danger"
    )

    def _card(label, value, css_class, status_key=None):
        inner = (
            f'<div class="card-label">{label}</div>'
            f'<div class="card-value">{value}</div>'
        )
        card_cls = f"card {css_class}" if css_class else "card"
        if status_key and section_prefix and int(value) > 0:
            href = f"#{section_prefix}-{status_key}-details"
            return (
                f'<a href="{href}" class="card-link">'
                f'<div class="{card_cls}">{inner}</div></a>'
            )
        return f'<div class="{card_cls}">{inner}</div>'

    return (
        f'<div class="section"><h2>{title}</h2><div class="card-grid">'
        + _card("Total Tests", stats["total"], "")
        + _card("Passed", stats["passed"], "card-success")
        + _card("Failed", stats["failed"], "card-danger", "failed")
        + _card("Error", stats.get("error", 0), "card-danger", "error")
        + _card("Skipped", stats["skipped"], "card-warning", "skipped")
        + f'<div class="card {rate_cls}"><div class="card-label">Pass Rate</div>'
        f'<div class="card-value">{rate:.1f}%</div></div>'
        "</div></div>"
    )


def _generate_status_detail_sections(data, prefix):
    """Generate collapsible sections listing tests by status (failed/error/skipped)."""
    status_tests = {"failed": [], "error": [], "skipped": []}
    for d in data:
        for tc in d.get("test_cases", []):
            key = tc.get("result", "").lower()
            if key in status_tests:
                status_tests[key].append((d, tc))

    status_meta = [
        ("failed", "Failed Tests", "toggle-danger"),
        ("error", "Error Tests", "toggle-danger"),
        ("skipped", "Skipped Tests", "toggle-warning"),
    ]

    # Build buttons row and content panels separately
    buttons = ""
    panels = ""
    for status_key, label, btn_cls in status_meta:
        tests = status_tests[status_key]
        if not tests:
            continue
        section_id = f"{prefix}-{status_key}-details"
        content_id = f"{section_id}-content"
        buttons += (
            f'<button class="toggle-btn {btn_cls}" '
            f"onclick=\"toggleDetails('{content_id}', this)\" "
            f'aria-expanded="false">{label} ({len(tests)})</button>'
        )
        panels += (
            f'<div id="{content_id}" class="details">'
            "<table><thead><tr>"
            "<th>NIC</th><th>Category</th><th>Test Name</th>"
            "<th>Duration</th><th>Log</th>"
            "</tr></thead><tbody>"
        )
        for idx, (d, tc) in enumerate(tests):
            log_id = f"{section_id}-log-{idx}"
            log_cell = _log_toggle_span(tc, log_id)
            panels += (
                f"<tr><td>{d.get('nic', '')}</td>"
                f"<td>{d.get('category', '')}</td>"
                f"<td>{tc.get('test_name', '')}</td>"
                f"<td>{tc.get('duration', '')}</td>"
                f"<td>{log_cell}</td></tr>"
            )
            panels += _log_row(tc, log_id, colspan=5)
        panels += "</tbody></table></div>"

    if not buttons:
        return ""
    return f'<div class="status-buttons-row">{buttons}</div>' f"{panels}"


# ---------------------------------------------------------------------------
# Section generators
# ---------------------------------------------------------------------------


def _generate_test_run_info(test_metadata):
    """Generate test run information table."""
    if not test_metadata:
        return ""

    rows = ""
    for prefix, label in [("pytest", "Pytest"), ("gtest", "GTest")]:
        run_num = test_metadata.get(f"{prefix}_run_number")
        branch = test_metadata.get(f"{prefix}_branch")
        if not run_num or not branch:
            continue
        raw = test_metadata.get(f"{prefix}_run_date", "")
        if raw:
            parts = raw.split("T")
            date = parts[0]
            time = parts[1].rstrip("Z") if len(parts) > 1 else "N/A"
        else:
            date, time = "N/A", "N/A"
        url = test_metadata.get(f"{prefix}_run_url")
        link = (
            f'<a href="{url}" target="_blank">#{run_num}</a>' if url else f"#{run_num}"
        )
        bl_num = test_metadata.get(f"baseline_{prefix}_run_number")
        bl_url = test_metadata.get(f"baseline_{prefix}_run_url")
        bl_branch = test_metadata.get(f"baseline_{prefix}_branch", "")
        if bl_num:
            bl_link = (
                f'<a href="{bl_url}" target="_blank">#{bl_num}</a>'
                if bl_url
                else f"#{bl_num}"
            )
            baseline = f"{bl_link} ({bl_branch})" if bl_branch else bl_link
        else:
            baseline = "&mdash;"
        rows += (
            f"<tr><td>{label}</td><td>{link}</td>"
            f"<td>{branch}</td><td>{date} {time}</td>"
            f"<td>{baseline}</td></tr>"
        )

    if not rows:
        return ""
    return (
        '<div class="section"><h2>Test Run Information</h2>'
        "<table><thead><tr><th>Test Suite</th><th>Run</th><th>Branch</th>"
        "<th>Date &amp; Time</th><th>Regression Baseline</th></tr></thead><tbody>"
        f"{rows}</tbody></table></div>"
    )


def _generate_system_info(system_info_list):
    """Generate test environment information section."""
    rows = ""
    for si in system_info_list:
        rows += (
            f"<tr><td>{si.get('hostname', 'unknown')}</td>"
            f"<td>{si.get('cpu', 'unknown')}</td>"
            f"<td>{si.get('cpu_cores', 'unknown')}</td>"
            f"<td>{si.get('hugepages', 'unknown')}</td>"
            f"<td>{si.get('os', 'unknown')}</td>"
            f"<td>{si.get('kernel', 'unknown')}</td>"
            f"<td>{si.get('nics', 'unknown')}</td></tr>"
        )
    return (
        '<div class="section"><h2>Test Environment</h2>'
        "<table><thead><tr><th>Hostname</th><th>CPU</th><th>Cores</th>"
        "<th>HugePages</th><th>OS</th><th>Kernel</th><th>NICs</th>"
        "</tr></thead><tbody>"
        f"{rows}</tbody></table></div>"
    )


def _generate_regression_section(regression_data):
    """Generate regression analysis section with collapsible detail tables."""
    regressions = regression_data.get("regressions", [])
    fixes = regression_data.get("fixes", [])
    new_failures = regression_data.get("new_failures", [])

    html = '<div class="section"><h2>Regression Analysis</h2>'
    html += (
        '<div class="card-grid">'
        f'<div class="card card-danger"><div class="card-label">Regressions</div>'
        f'<div class="card-value">{len(regressions)}</div></div>'
        f'<div class="card card-success"><div class="card-label">Fixes</div>'
        f'<div class="card-value">{len(fixes)}</div></div>'
        f'<div class="card card-warning"><div class="card-label">New Failures</div>'
        f'<div class="card-value">{len(new_failures)}</div></div>'
        "</div>"
    )

    coverage = regression_data.get("coverage")
    if coverage:
        cur = coverage["current_total"]
        base = coverage["baseline_total"]
        delta = cur - base
        sign = "+" if delta > 0 else ""
        html += (
            '<div style="margin:10px 0;padding:8px 14px;'
            "background:#f0f4f8;border-left:4px solid #4a90d9;"
            'border-radius:4px;font-size:0.92em;color:#333">'
            f"<strong>Coverage:</strong> current run has <strong>{cur}</strong> "
            f"tests, baseline had <strong>{base}</strong> ({sign}{delta}). "
            f"{coverage['common']} in common, {coverage['only_in_current']} new "
            f"in current, {coverage['only_in_baseline']} absent from current."
            "</div>"
        )

    for key, label, subtitle, show_baseline, modifier in _REGRESSION_SECTIONS:
        entries = regression_data.get(key, [])
        if not entries:
            continue
        detail_id = f"{key}-details"
        html += (
            f'<button class="toggle-btn toggle-{modifier}" '
            f"onclick=\"toggleDetails('{detail_id}', this)\" "
            f'aria-expanded="false">{label} ({len(entries)})</button>'
            f'<div id="{detail_id}" class="details">'
            f"<h4>{label} &mdash; {subtitle}</h4>"
        )
        html += _build_regression_table(entries, show_baseline)
        html += "</div>"

    html += "</div>"
    return html


def _build_regression_table(entries, show_baseline):
    """Render a regression/fix/new-failure detail table."""
    html = (
        "<table><thead><tr><th>Platform</th><th>NIC</th>"
        "<th>Category</th><th>Test Name</th>"
    )
    if show_baseline:
        html += "<th>Baseline</th>"
    html += "<th>Current</th></tr></thead><tbody>"

    for idx, e in enumerate(entries):
        log = e.get("log", "")
        log_id = f"reglog-{e['nic']}-{idx}"
        log_cell = ""
        if log:
            log_cell = (
                f' <span class="log-toggle" '
                f"onclick=\"toggleDetails('{log_id}', this)\">Show log</span>"
            )
        html += (
            f"<tr><td>{e['platform']}</td><td>{e['nic']}</td>"
            f"<td>{e['category']}</td><td>{e['test_name']}{log_cell}</td>"
        )
        if show_baseline:
            html += (
                f'<td class="{_result_class(e.get("baseline_result", ""))}">'
                f'{e.get("baseline_result", "N/A")}</td>'
            )
        html += (
            f'<td class="{_result_class(e["current_result"])}">'
            f'{e["current_result"]}</td></tr>'
        )
        if log:
            col_span = 6 if show_baseline else 5
            html += (
                f'<tr class="log-row"><td colspan="{col_span}">'
                f'<pre id="{log_id}" class="log-content">'
                f"{_escape_html(log)}</pre></td></tr>"
            )

    html += "</tbody></table>"
    return html


# ---------------------------------------------------------------------------
# Donut charts
# ---------------------------------------------------------------------------


def _svg_donut(passed, failed, skipped, size=100, stroke=14, error=0, xfailed=0):
    """Return an inline SVG donut chart.

    Shows pass/fail/error/xfailed segments (skipped excluded from chart).
    Displays (passed+xfailed)/(passed+failed+error+xfailed) as percentage.
    Renders a grey placeholder when no countable tests exist.
    """
    total = passed + failed + error + xfailed
    r = (size - stroke) / 2
    cx = cy = size / 2

    if total == 0:
        return (
            f'<svg width="{size}" height="{size}" viewBox="0 0 {size} {size}">'
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" '
            f'stroke="#e0e0e0" stroke-width="{stroke}"/>'
            f'<text x="{cx}" y="{cy}" text-anchor="middle" '
            f'dominant-baseline="central" '
            f'font-size="14" font-weight="700" fill="#9aa0a6">N/A</text></svg>'
        )

    circ = 2 * pi * r
    rate = (passed + xfailed) / total * 100
    rate_color = "#137333" if rate >= 95 else "#7c6900" if rate >= 80 else "#a50e0e"

    segments = []
    offset = 0
    for count, color in [
        (passed, _CHART_COLORS["passed"]),
        (failed, _CHART_COLORS["failed"]),
        (error, _CHART_COLORS["error"]),
        (xfailed, _CHART_COLORS["xfailed"]),
    ]:
        if count == 0:
            continue
        dash = count / total * circ
        segments.append(
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" '
            f'stroke="{color}" stroke-width="{stroke}" '
            f'stroke-dasharray="{dash:.2f} {circ - dash:.2f}" '
            f'stroke-dashoffset="{-offset:.2f}" '
            f'transform="rotate(-90 {cx} {cy})"/>'
        )
        offset += dash

    return (
        f'<svg width="{size}" height="{size}" viewBox="0 0 {size} {size}">'
        + "".join(segments)
        + f'<text x="{cx}" y="{cy}" text-anchor="middle" '
        f'dominant-baseline="central" '
        f'font-size="16" font-weight="700" fill="{rate_color}">'
        f"{rate:.0f}%</text></svg>"
    )


def _aggregate_by_nic_category(data):
    """Aggregate test data counts by (nic, category) key."""
    agg = defaultdict(
        lambda: {
            "passed": 0,
            "failed": 0,
            "skipped": 0,
            "error": 0,
            "xfailed": 0,
            "total": 0,
        }
    )
    for d in data:
        key = (d.get("nic", "unknown"), d.get("category", "unknown"))
        for field in ("passed", "failed", "skipped", "error", "xfailed", "total"):
            agg[key][field] += d.get(field, 0)
    return agg


def _render_chart_items(agg, nic):
    """Render donut chart items for a single NIC from aggregated data."""
    html = ""
    for key in sorted(k for k in agg if k[0] == nic):
        s = agg[key]
        donut = _svg_donut(
            s["passed"],
            s["failed"],
            s["skipped"],
            error=s["error"],
            xfailed=s["xfailed"],
        )
        html += (
            f'<div class="chart-item"><div>{donut}</div>'
            f'<div class="chart-label">{key[1]}</div>'
            f'<div class="chart-counts">'
            f'<span class="cnt-pass">{s["passed"]}</span> / '
            f'<span class="cnt-fail">{s["failed"]}</span> / '
            f'<span class="cnt-skip">{s["skipped"]}</span>'
            f"</div></div>"
        )
    return html


def _generate_chart_grid(data):
    """Build a grid of donut charts, one per NIC per category."""
    agg = _aggregate_by_nic_category(data)
    nics = sorted(set(k[0] for k in agg))
    html = ""
    for nic in nics:
        html += (
            f'<h3 style="font-size:13px;color:#366092;margin:12px 0 6px;">'
            f"{nic.upper()}</h3>"
            '<div class="chart-grid">'
        )
        html += _render_chart_items(agg, nic)
        html += "</div>"
    return html


# ---------------------------------------------------------------------------
# Application extraction & charts
# ---------------------------------------------------------------------------


def _extract_application(test_name):
    """Extract application name from '|application = <name>|' in test name."""
    m = _APPLICATION_RE.search(test_name)
    return m.group(1) if m else "unknown"


def _split_data_by_application(data):
    """Split data into per-application groups based on test_cases.

    Returns {app_name: [entries]} with recalculated per-app counts.
    """
    app_data = defaultdict(list)

    for d in data:
        test_cases = d.get("test_cases", [])
        if not test_cases:
            app_data["unknown"].append(d)
            continue

        app_cases = defaultdict(list)
        for tc in test_cases:
            app_cases[_extract_application(tc.get("test_name", ""))].append(tc)

        for app, cases in app_cases.items():
            app_data[app].append(
                {
                    "nic": d["nic"],
                    "category": d["category"],
                    "runner": d.get("runner", "unknown"),
                    "application": app,
                    "passed": sum(1 for tc in cases if tc["result"] == "PASSED"),
                    "failed": sum(1 for tc in cases if tc["result"] == "FAILED"),
                    "error": sum(1 for tc in cases if tc["result"] == "ERROR"),
                    "skipped": sum(1 for tc in cases if tc["result"] == "SKIPPED"),
                    "xpassed": sum(1 for tc in cases if tc["result"] == "XPASS"),
                    "xfailed": sum(1 for tc in cases if tc["result"] == "XFAIL"),
                    "total": len(cases),
                    "test_cases": cases,
                }
            )

    return dict(app_data)


def _sorted_app_keys(app_groups):
    """Sort application keys by preferred order, then alphabetically."""
    return sorted(
        app_groups.keys(),
        key=lambda a: (_APP_ORDER.index(a) if a in _APP_ORDER else 999, a),
    )


def _generate_app_chart_sections(data):
    """Generate chart sections grouped by Application, then NIC and Category."""
    app_groups = _split_data_by_application(data)
    html = ""

    for app in _sorted_app_keys(app_groups):
        entries = app_groups[app]
        app_label = app.capitalize() if app != "unknown" else "Other"
        agg = _aggregate_by_nic_category(entries)
        nics = sorted(set(k[0] for k in agg))

        html += (
            f'<div style="margin-bottom:18px;">'
            f'<h3 style="font-size:14px;color:#366092;margin-bottom:4px;">'
            f"{app_label}</h3>"
        )
        for nic in nics:
            html += (
                f'<h4 style="font-size:13px;color:#5f6368;margin:10px 0 6px;">'
                f"{nic.upper()}</h4>"
                '<div class="chart-grid">'
            )
            html += _render_chart_items(agg, nic)
            html += "</div>"
        html += "</div>"

    return html


# ---------------------------------------------------------------------------
# Test result tables
# ---------------------------------------------------------------------------


def _generate_test_table(data, prefix, columns, include_application=False):
    """Generate a test results table grouped by category with NIC breakdown."""
    if include_application:
        return _generate_test_table_with_app(data, prefix, columns)

    col_count = len(columns)
    html = _table_header(columns)

    sorted_data = sorted(data, key=lambda x: (x["category"], x["nic"]))
    idx = 0
    for category, group in groupby(sorted_data, key=lambda x: x["category"]):
        html += _category_header_row(category, col_count)
        for d in list(group):
            detail_id = f"{prefix}-detail-{idx}"
            idx += 1
            html += _data_row(d, columns, detail_id)
            html += _detail_section(d, detail_id, col_count)

    html += "</tbody></table>"
    return html


def _generate_test_table_with_app(data, prefix, columns):
    """Generate table grouped by Application -> Category -> NIC."""
    app_groups = _split_data_by_application(data)
    app_columns = [("Application", "application")] + list(columns)
    col_count = len(app_columns)

    html = _table_header(app_columns)
    idx = 0

    for app in _sorted_app_keys(app_groups):
        entries = app_groups[app]
        app_label = app.capitalize() if app != "unknown" else "Other"

        html += (
            f'<tr class="category-group">'
            f'<td colspan="{col_count}" style="background:#dbe5f1;">'
            f"<strong>{app_label}</strong></td></tr>"
        )

        sorted_entries = sorted(entries, key=lambda x: (x["category"], x["nic"]))
        for category, cat_group in groupby(sorted_entries, key=lambda x: x["category"]):
            html += _category_header_row(category, col_count)
            for d in list(cat_group):
                detail_id = f"{prefix}-detail-{idx}"
                idx += 1
                html += _data_row(d, columns, detail_id, app_prefix=app_label)
                html += _detail_section(
                    d,
                    detail_id,
                    col_count,
                    title_prefix=f"{app_label} &mdash; ",
                )

    html += "</tbody></table>"
    return html


def _table_header(columns):
    """Generate table opening with header row."""
    html = "<table><thead><tr>"
    for header, _ in columns:
        html += f"<th>{header}</th>"
    html += "</tr></thead><tbody>"
    return html


def _category_header_row(category, col_count):
    """Generate a category group header row."""
    return (
        f'<tr class="category-group">' f'<td colspan="{col_count}">{category}</td></tr>'
    )


def _data_row(d, columns, detail_id, app_prefix=None):
    """Generate a clickable data row."""
    html = (
        f"<tr onclick=\"toggleDetails('{detail_id}', this)\" "
        f'style="cursor:pointer" aria-expanded="false">'
    )
    if app_prefix is not None:
        html += f"<td>{app_prefix}</td>"
    for _, key in columns:
        val = d.get(key, 0)
        if key == "category":
            html += f'<td><span class="category-pill">{val}</span></td>'
        else:
            html += f"<td>{val}</td>"
    html += "</tr>"
    return html


def _detail_section(d, detail_id, col_count, title_prefix=""):
    """Generate collapsible detail section with individual test case results."""
    test_cases = d.get("test_cases", [])
    if not test_cases:
        return ""

    html = (
        f'<tr><td colspan="{col_count}">'
        f'<div id="{detail_id}" class="details">'
        f"<h4>{title_prefix}{d['nic']} &mdash; {d['category']} &mdash; "
        f"Detailed Results</h4>"
        "<table><thead><tr><th>Test Name</th><th>Result</th>"
        "<th>Duration</th></tr></thead><tbody>"
    )
    for tc in sorted(test_cases, key=lambda x: x["test_name"]):
        log_id = f"{detail_id}-log-{tc['test_name']}"
        log_cell = _log_toggle_span(tc, log_id)
        html += (
            f'<tr><td>{tc["test_name"]}{log_cell}</td>'
            f'<td class="{_result_class(tc["result"])}">{tc["result"]}</td>'
            f'<td>{tc["duration"]}</td></tr>'
        )
        html += _log_row(tc, log_id, colspan=3)
    html += "</tbody></table></div></td></tr>"
    return html


def _generate_pytest_table(pytest_data):
    """Generate pytest results section with application-grouped charts."""
    charts = _generate_app_chart_sections(pytest_data)
    status_details = _generate_status_detail_sections(pytest_data, "pytest")
    table = _generate_test_table(
        pytest_data, "pytest", _PYTEST_COLUMNS, include_application=True
    )
    return (
        '<div class="section">'
        "<h2>Pytest Results by Application, NIC and Category</h2>"
        f"{charts}{status_details}{table}</div>"
    )


def _generate_gtest_table(gtest_data):
    """Generate gtest results section with category donut charts."""
    charts = _generate_chart_grid(gtest_data)
    status_details = _generate_status_detail_sections(gtest_data, "gtest")
    table = _generate_test_table(gtest_data, "gtest", _GTEST_COLUMNS)
    return (
        '<div class="section">'
        "<h2>GTest Results by NIC and Test Category</h2>"
        f"{charts}{status_details}{table}</div>"
    )


# ---------------------------------------------------------------------------
# Utility helpers
# ---------------------------------------------------------------------------


def _log_toggle_span(tc, log_id):
    """Return a 'Show log' toggle span if the test case has displayable log."""
    if not tc.get("log") or tc["result"] not in _LOG_STATUSES:
        return ""
    return (
        f' <span class="log-toggle" '
        f'onclick="event.stopPropagation(); '
        f"toggleDetails('{log_id}', this)\">Show log</span>"
    )


def _log_row(tc, log_id, colspan):
    """Return a hidden log row if the test case has displayable log."""
    if not tc.get("log") or tc["result"] not in _LOG_STATUSES:
        return ""
    return (
        f'<tr class="log-row"><td colspan="{colspan}">'
        f'<pre id="{log_id}" class="log-content">'
        f"{_escape_html(tc['log'])}</pre></td></tr>"
    )


def _result_class(result):
    """Return CSS class name for a test result string."""
    if result and result.lower() in ("passed", "failed", "skipped"):
        return f"result-{result.lower()}"
    return ""


def _escape_html(text):
    """Escape HTML special characters."""
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


# ---------------------------------------------------------------------------
# HTML template
# ---------------------------------------------------------------------------


def _get_html_template():
    """Return the HTML template with CSS and JavaScript."""
    return """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8"/>
    <meta name="viewport" content="width=device-width, initial-scale=1"/>
    <title>MTL Nightly Test Report</title>
    <style>
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #f0f2f5; color: #1d1d1f; line-height: 1.5;
        }}
        header {{
            background: linear-gradient(135deg, #1a3a5c 0%, #366092 100%);
            color: #fff; padding: 32px 40px; margin-bottom: 24px;
        }}
        header h1 {{ font-size: 24px; font-weight: 600; margin-bottom: 4px; }}
        .subtitle {{ font-size: 14px; opacity: .8; }}
        .timestamp {{ font-size: 12px; opacity: .6; margin-top: 8px; }}
        main {{ max-width: 1280px; margin: 0 auto; padding: 0 24px 40px; }}
        .section {{
            background: #fff; border-radius: 8px; padding: 24px;
            margin-bottom: 20px; box-shadow: 0 1px 3px rgba(0,0,0,.08);
        }}
        h2 {{
            font-size: 16px; font-weight: 600; color: #1a3a5c;
            margin-bottom: 16px; padding-bottom: 8px;
            border-bottom: 2px solid #e8ecf0;
        }}
        .card-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
            gap: 12px;
        }}
        .card {{
            padding: 16px; border-radius: 8px; text-align: center;
            background: #f8f9fa; border-left: 4px solid #dadce0;
        }}
        .card-label {{
            font-size: 11px; color: #5f6368;
            text-transform: uppercase; letter-spacing: .5px;
        }}
        .card-value {{ font-size: 28px; font-weight: 700; margin-top: 4px; }}
        .card-success {{ background: #e6f4ea; border-left-color: #34a853; color: #137333; }}
        .card-danger  {{ background: #fce8e6; border-left-color: #ea4335; color: #a50e0e; }}
        .card-warning {{ background: #fef7e0; border-left-color: #f9ab00; color: #7c6900; }}
        .card-link {{
            text-decoration: none; color: inherit; display: block;
            border-radius: 8px; transition: transform .1s ease, box-shadow .1s ease;
        }}
        .card-link:hover {{
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(0,0,0,.12);
        }}
        .card-link .card {{ cursor: pointer; }}
        table {{ width: 100%; border-collapse: collapse; font-size: 14px; }}
        thead th {{
            background: #1a3a5c; color: #fff; padding: 10px 14px;
            text-align: left; font-weight: 500; font-size: 13px;
        }}
        tbody td {{ padding: 9px 14px; border-bottom: 1px solid #e8ecf0; }}
        tbody tr:hover {{ background: #f8f9fa; }}
        .result-passed  {{ background: #e6f4ea; color: #137333; font-weight: 500; }}
        .result-failed  {{ background: #fce8e6; color: #a50e0e; font-weight: 500; }}
        .result-skipped {{ background: #fef7e0; color: #7c6900; }}
        .category-pill {{
            display: inline-block; background: #e8f0fe; color: #1a73e8;
            padding: 3px 14px; border-radius: 14px; cursor: pointer;
            font-weight: 500; font-size: 13px; transition: all .15s ease;
        }}
        .category-pill:hover,
        .category-pill[aria-expanded="true"] {{ background: #1a73e8; color: #fff; }}
        .toggle-btn {{
            display: inline-flex; align-items: center; gap: 6px;
            padding: 8px 20px; border: none; border-radius: 6px;
            cursor: pointer; font-size: 13px; font-weight: 500;
            margin: 10px 6px 4px 0; transition: all .15s ease; color: #fff;
        }}
        .toggle-btn::before {{ content: "\u25b6 "; font-size: 10px; }}
        .toggle-btn[aria-expanded="true"]::before {{ content: "\u25bc "; }}
        .status-buttons-row {{ display: flex; flex-wrap: wrap; gap: 8px; margin: 12px 0; }}
        .toggle-danger  {{ background: #ea4335; }}
        .toggle-danger:hover  {{ background: #c5221f; }}
        .toggle-warning {{ background: #f9ab00; color: #1d1d1f; }}
        .toggle-warning:hover {{ background: #e69500; }}
        .toggle-success {{ background: #34a853; }}
        .toggle-success:hover {{ background: #1e8e3e; }}
        .details {{
            display: none; margin-top: 8px; padding: 12px;
            background: #f8f9fa; border-left: 3px solid #366092;
            border-radius: 0 6px 6px 0;
        }}
        .details.show {{ display: block; }}
        .details h4 {{ font-size: 14px; color: #366092; margin-bottom: 10px; }}
        .details table {{ font-size: 13px; }}
        .details thead th {{ background: #5f6368; font-size: 12px; }}
        .category-group {{ margin-top: 4px; }}
        .category-group td {{
            background: #eef3f8; font-weight: 600; color: #1a3a5c;
        }}
        .chart-grid {{
            display: flex; flex-wrap: wrap; gap: 20px;
            margin-bottom: 20px; padding: 16px 0;
        }}
        .chart-item {{ text-align: center; min-width: 110px; }}
        .chart-label {{
            font-size: 12px; font-weight: 600; color: #1a3a5c;
            margin-top: 6px; text-transform: capitalize;
        }}
        .chart-counts {{ font-size: 11px; color: #5f6368; margin-top: 2px; }}
        .cnt-pass {{ color: #137333; }}
        .cnt-fail {{ color: #a50e0e; }}
        .cnt-skip {{ color: #7c6900; }}
        a {{ color: #1a73e8; text-decoration: none; }}
        a:hover {{ text-decoration: underline; }}
        .log-toggle {{
            color: #1a73e8; cursor: pointer; font-size: 12px;
            text-decoration: underline; user-select: none;
        }}
        .log-toggle:hover {{ color: #174ea6; }}
        .log-row td {{ padding: 0 !important; border-bottom: none; }}
        .log-content {{
            display: none; margin: 0; padding: 10px 14px;
            background: #1e1e1e; color: #d4d4d4;
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 12px; line-height: 1.5;
            white-space: pre-wrap; word-break: break-all;
            max-height: 400px; overflow-y: auto;
            border-left: 3px solid #ea4335;
        }}
        .log-content.show {{ display: block; }}
        footer {{
            text-align: center; padding: 24px; color: #9aa0a6; font-size: 12px;
        }}
    </style>
    <script>
        function toggleDetails(id, trigger) {{
            var el = document.getElementById(id);
            if (!el) return;
            var isOpen = el.classList.toggle('show');
            if (trigger) trigger.setAttribute('aria-expanded', isOpen);
        }}
    </script>
</head>
<body>
    <header>
        <h1>MTL Nightly Test Report</h1>
        <div class="subtitle">Combined Pytest &amp; GTest Results</div>
        <div class="timestamp">Generated: {timestamp}</div>
    </header>
    <main>
        {test_run_info}
        {system_info_section}
        {overall_summary}
        {pytest_summary}
        {gtest_summary}
        {regression_section}
        {pytest_table}
        {gtest_table}
    </main>
    <footer>Media Transport Library &mdash; Nightly Test Report</footer>
</body>
</html>"""
