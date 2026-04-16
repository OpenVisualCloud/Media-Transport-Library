# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""HTML report generation for MTL nightly test reports."""

from collections import defaultdict
from datetime import datetime
from itertools import groupby

# Column definitions for test result tables: (header_label, data_key)
_PYTEST_COLUMNS = [
    ("NIC", "nic"),
    ("Category", "category"),
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
        overall_summary=_generate_summary_cards("Overall Summary", stats["combined"]),
        pytest_summary=(
            _generate_summary_cards("Pytest Summary", stats["pytest"])
            if pytest_data
            else ""
        ),
        gtest_summary=(
            _generate_summary_cards("GTest Summary", stats["gtest"])
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
                "pass_rate": 0,
            }
        passed = sum(d.get("passed", 0) for d in data)
        failed = sum(d.get("failed", 0) for d in data)
        return {
            "total": sum(d.get("total", 0) for d in data),
            "passed": passed,
            "failed": failed,
            "skipped": sum(d.get("skipped", 0) for d in data),
            "pass_rate": (
                passed / (passed + failed) * 100 if (passed + failed) > 0 else 0
            ),
        }

    p = _suite_stats(pytest_data)
    g = _suite_stats(gtest_data)
    cp = p["passed"] + g["passed"]
    cf = p["failed"] + g["failed"]
    return {
        "pytest": p,
        "gtest": g,
        "combined": {
            "total": p["total"] + g["total"],
            "passed": cp,
            "failed": cf,
            "skipped": p["skipped"] + g["skipped"],
            "pass_rate": cp / (cp + cf) * 100 if (cp + cf) > 0 else 0,
        },
    }


# ---------------------------------------------------------------------------
# Section generators
# ---------------------------------------------------------------------------


def _format_run_datetime(metadata, prefix):
    """Extract formatted date and time strings from metadata."""
    raw = metadata.get(f"{prefix}_run_date", "")
    if not raw:
        return "N/A", "N/A"
    parts = raw.split("T")
    return parts[0], parts[1].rstrip("Z") if len(parts) > 1 else "N/A"


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
        date, time = _format_run_datetime(test_metadata, prefix)
        url = test_metadata.get(f"{prefix}_run_url")
        link = (
            f'<a href="{url}" target="_blank">#{run_num}</a>' if url else f"#{run_num}"
        )
        # Baseline column
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


def _generate_summary_cards(title, stats):
    """Generate a summary card grid for any test suite."""
    rate = stats["pass_rate"]
    rate_cls = (
        "card-success"
        if rate >= 95
        else "card-warning" if rate >= 80 else "card-danger"
    )
    return (
        f'<div class="section"><h2>{title}</h2><div class="card-grid">'
        f'<div class="card"><div class="card-label">Total Tests</div>'
        f'<div class="card-value">{stats["total"]}</div></div>'
        f'<div class="card card-success"><div class="card-label">Passed</div>'
        f'<div class="card-value">{stats["passed"]}</div></div>'
        f'<div class="card card-danger"><div class="card-label">Failed</div>'
        f'<div class="card-value">{stats["failed"]}</div></div>'
        f'<div class="card card-warning"><div class="card-label">Skipped</div>'
        f'<div class="card-value">{stats["skipped"]}</div></div>'
        f'<div class="card {rate_cls}"><div class="card-label">Pass Rate</div>'
        f'<div class="card-value">{rate:.1f}%</div></div>'
        "</div></div>"
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
        html += _generate_coverage_note(coverage)

    for key, label, subtitle, show_baseline, modifier in _REGRESSION_SECTIONS:
        entries = regression_data.get(key, [])
        if not entries:
            continue
        detail_id = f"{key}-details"
        html += (
            f'<button class="toggle-btn toggle-{modifier}" '
            f"onclick=\"toggleDetails('{detail_id}', this)\" "
            f'aria-expanded="false">{label} ({len(entries)})</button>'
        )
        html += f'<div id="{detail_id}" class="details">'
        html += f"<h4>{label} &mdash; {subtitle}</h4>"
        html += _build_regression_table(entries, show_baseline)
        html += "</div>"

    html += "</div>"
    return html


def _generate_coverage_note(coverage):
    """Generate a coverage comparison note for the regression section."""
    cur = coverage["current_total"]
    base = coverage["baseline_total"]
    common = coverage["common"]
    only_cur = coverage["only_in_current"]
    only_base = coverage["only_in_baseline"]
    delta = cur - base
    sign = "+" if delta > 0 else ""
    return (
        '<div style="margin:10px 0;padding:8px 14px;'
        "background:#f0f4f8;border-left:4px solid #4a90d9;"
        'border-radius:4px;font-size:0.92em;color:#333">'
        f"<strong>Coverage:</strong> current run has <strong>{cur}</strong> tests, "
        f"baseline had <strong>{base}</strong> ({sign}{delta}). "
        f"{common} in common, {only_cur} new in current, "
        f"{only_base} absent from current."
        "</div>"
    )


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
        log_cell = ""
        if log:
            log_id = f"reglog-{e['nic']}-{idx}"
            log_cell = (
                f' <span class="log-toggle" '
                f"onclick=\"toggleDetails('{log_id}', this)\">"
                f"Show log</span>"
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
            escaped_log = _escape_html(log)
            html += (
                f'<tr class="log-row"><td colspan="{col_span}">'
                f'<pre id="{log_id}" class="log-content">'
                f"{escaped_log}</pre></td></tr>"
            )

    html += "</tbody></table>"
    return html


def _result_class(result):
    """Return CSS class name for a test result string."""
    if result and result.lower() in ("passed", "failed", "skipped"):
        return f"result-{result.lower()}"
    return ""


def _escape_html(text):
    """Escape HTML special characters in log text."""
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


# ---------------------------------------------------------------------------
# Donut charts
# ---------------------------------------------------------------------------

# SVG donut chart colors: (key, color)
_CHART_COLORS = {
    "passed": "#34a853",
    "failed": "#ea4335",
    "skipped": "#f9ab00",
}


def _svg_donut(passed, failed, skipped, size=100, stroke=14):
    """Return an inline SVG donut chart with pass/fail/skip segments."""
    total = passed + failed + skipped
    if total == 0:
        return ""

    r = (size - stroke) / 2
    circ = 2 * 3.14159265 * r
    cx = cy = size / 2
    rate = passed / (passed + failed) * 100 if (passed + failed) > 0 else 0
    rate_color = "#137333" if rate >= 95 else "#7c6900" if rate >= 80 else "#a50e0e"

    segments = []
    offset = 0
    for count, color in [
        (passed, _CHART_COLORS["passed"]),
        (failed, _CHART_COLORS["failed"]),
        (skipped, _CHART_COLORS["skipped"]),
    ]:
        if count == 0:
            continue
        dash = count / total * circ
        gap = circ - dash
        segments.append(
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" '
            f'stroke="{color}" stroke-width="{stroke}" '
            f'stroke-dasharray="{dash:.2f} {gap:.2f}" '
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


def _generate_chart_grid(data):
    """Build a grid of donut charts, one per unique category."""
    # Aggregate across NICs per category
    agg = defaultdict(lambda: {"passed": 0, "failed": 0, "skipped": 0, "total": 0})
    for d in data:
        cat = d.get("category", "unknown")
        agg[cat]["passed"] += d.get("passed", 0)
        agg[cat]["failed"] += d.get("failed", 0)
        agg[cat]["skipped"] += d.get("skipped", 0)
        agg[cat]["total"] += d.get("total", 0)

    html = '<div class="chart-grid">'
    for cat in sorted(agg):
        s = agg[cat]
        donut = _svg_donut(s["passed"], s["failed"], s["skipped"])
        html += (
            f'<div class="chart-item">'
            f"<div>{donut}</div>"
            f'<div class="chart-label">{cat}</div>'
            f'<div class="chart-counts">'
            f'<span class="cnt-pass">{s["passed"]}</span> / '
            f'<span class="cnt-fail">{s["failed"]}</span> / '
            f'<span class="cnt-skip">{s["skipped"]}</span>'
            f"</div></div>"
        )
    html += "</div>"
    return html


# ---------------------------------------------------------------------------
# Test result tables
# ---------------------------------------------------------------------------


def _generate_test_table(data, prefix, columns):
    """Generate a test results table grouped by category with NIC breakdown."""
    col_count = len(columns)
    html = "<table><thead><tr>"
    for header, _ in columns:
        html += f"<th>{header}</th>"
    html += "</tr></thead><tbody>"

    # Group by category, then by NIC within each category
    sorted_data = sorted(data, key=lambda x: (x["category"], x["nic"]))
    idx = 0
    for category, group in groupby(sorted_data, key=lambda x: x["category"]):
        entries = list(group)
        # Category group header row
        html += (
            f'<tr class="category-group">'
            f'<td colspan="{col_count}">{category}</td></tr>'
        )

        for d in entries:
            detail_id = f"{prefix}-detail-{idx}"
            idx += 1
            html += "<tr>"
            for _, key in columns:
                val = d.get(key, 0)
                if key == "category":
                    html += (
                        f'<td><span class="category-pill" '
                        f"onclick=\"toggleDetails('{detail_id}', this)\" "
                        f'aria-expanded="false">{val}</span></td>'
                    )
                else:
                    html += f"<td>{val}</td>"
            html += "</tr>"

            test_cases = d.get("test_cases", [])
            if test_cases:
                html += (
                    f'<tr><td colspan="{col_count}">'
                    f'<div id="{detail_id}" class="details">'
                    f'<h4>{d["nic"]} &mdash; {d["category"]} &mdash; '
                    f"Detailed Results</h4>"
                    "<table><thead><tr><th>Test Name</th><th>Result</th>"
                    "<th>Duration</th></tr></thead><tbody>"
                )
                for tc in sorted(test_cases, key=lambda x: x["test_name"]):
                    cls = _result_class(tc["result"])
                    log = tc.get("log", "")
                    log_cell = ""
                    if log and tc["result"] in ("FAILED", "ERROR"):
                        log_id = f"{detail_id}-log-{tc['test_name']}"
                        log_cell = (
                            f' <span class="log-toggle" '
                            f"onclick=\"toggleDetails('{log_id}', this)\">"
                            f"Show log</span>"
                        )
                    html += (
                        f'<tr><td>{tc["test_name"]}{log_cell}</td>'
                        f'<td class="{cls}">{tc["result"]}</td>'
                        f'<td>{tc["duration"]}</td></tr>'
                    )
                    if log and tc["result"] in ("FAILED", "ERROR"):
                        escaped_log = _escape_html(log)
                        html += (
                            f'<tr class="log-row"><td colspan="3">'
                            f'<pre id="{log_id}" class="log-content">'
                            f"{escaped_log}</pre></td></tr>"
                        )
                html += "</tbody></table></div></td></tr>"

    html += "</tbody></table>"
    return html


def _generate_pytest_table(pytest_data):
    """Generate pytest results section with category donut charts."""
    charts = _generate_chart_grid(pytest_data)
    table = _generate_test_table(pytest_data, "pytest", _PYTEST_COLUMNS)
    return (
        '<div class="section">'
        "<h2>Pytest Results by NIC and Category</h2>"
        f"{charts}{table}</div>"
    )


def _generate_gtest_table(gtest_data):
    """Generate gtest results section with category donut charts."""
    charts = _generate_chart_grid(gtest_data)
    table = _generate_test_table(gtest_data, "gtest", _GTEST_COLUMNS)
    return (
        '<div class="section">'
        "<h2>GTest Results by NIC and Test Category</h2>"
        f"{charts}{table}</div>"
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
            margin-bottom: 16px; padding-bottom: 8px; border-bottom: 2px solid #e8ecf0;
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
        .category-group td {{ background: #eef3f8; font-weight: 600; color: #1a3a5c; }}
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
