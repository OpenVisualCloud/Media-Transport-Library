# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""The files of a finished run: summary.json, timeline.json, report.md, report.html."""

from __future__ import annotations

import dataclasses
import html
import json
import os

from .workload import Plan


def write_reports(
    run_dir: str, plan: Plan, summary: dict, timeline: dict, verdicts: list
) -> None:
    with open(os.path.join(run_dir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    with open(os.path.join(run_dir, "timeline.json"), "w", encoding="utf-8") as f:
        json.dump(timeline, f)
    md = markdown(plan, summary)
    with open(os.path.join(run_dir, "report.md"), "w", encoding="utf-8") as f:
        f.write(md)
    with open(os.path.join(run_dir, "report.html"), "w", encoding="utf-8") as f:
        f.write(to_html(plan, summary, verdicts))


def _streams_table(plan: Plan, summary: dict) -> list[list[str]]:
    rows = []
    for s in plan.streams:
        e = summary["ebu"].get(s.sid, {})
        ebu = (
            ", ".join(f"{k} {v}" for k, v in e.items() if v)
            if e
            else ("not captured" if s.ebu else "n/a")
        )
        fmt = (
            f"{s.width}x{s.height} {s.fps}"
            if s.media == "video"
            else (
                f"{s.audio_format} {s.audio_sampling} {s.channels}ch {s.audio_ptime} ms"
                if s.media == "audio"
                else s.fps
            )
        )
        rows.append(
            [
                s.sid,
                s.api,
                s.worker,
                fmt,
                "yes" if s.user_pacing else "no",
                f"{s.dip}:{s.port}",
                ebu,
                s.note,
            ]
        )
    return rows


STREAM_HEAD = [
    "Stream",
    "API",
    "Worker",
    "Format",
    "User pacing",
    "Destination",
    "EBU LIST",
    "Note",
]


def markdown(plan: Plan, summary: dict) -> str:
    out = [f"# MTL stability run: {'PASSED' if summary['passed'] else 'FAILED'}", ""]
    out.append(
        f"Profile `{summary['profile']}`, {summary['ran_s']:.0f} s of {summary['duration_s']} s."
    )
    out.append("")
    if summary["reasons"]:
        out += ["## Reasons", ""] + [f"- {r}" for r in summary["reasons"]] + [""]
    out += [
        "## Workers",
        "",
        "| Worker | Kind | Status | Ran (s) | Note |",
        "|---|---|---|---|---|",
    ]
    for n, w in summary["workers"].items():
        out.append(
            f"| {n} | {w['kind']} | {w['rc']}{' (early)' if w['stopped_early'] else ''} | {w['ran_s']} | {w['note']} |"
        )
    out += [
        "",
        "## Streams",
        "",
        "| " + " | ".join(STREAM_HEAD) + " |",
        "|" + "---|" * len(STREAM_HEAD),
    ]
    for r in _streams_table(plan, summary):
        out.append("| " + " | ".join(r) + " |")
    logs = summary["logs"]
    if logs["error_counts"]:
        out += [
            "",
            "## Log lines of an error class",
            "",
            "| Worker/class | Lines | First line |",
            "|---|---|---|",
        ]
        for k, n in logs["error_counts"].items():
            first = (logs["error_lines"].get(k) or [[0, ""]])[0]
            out.append(f"| {k} | {n} | `{first[1][:160].replace('|', '/')}` |")
    if logs["bad_counters"]:
        out += [
            "",
            "## Session counters above 0",
            "",
            "| Session/counter | Sum |",
            "|---|---|",
        ]
        out += [f"| {k} | {v} |" for k, v in logs["bad_counters"].items()]
    out += [
        "",
        "## Session fps",
        "",
        "| Session | Samples | Median | Min | Max |",
        "|---|---|---|---|---|",
    ]
    for k, v in sorted(logs["sessions"].items()):
        out.append(
            f"| {k} | {v['samples']} | {v['median_fps']} | {v['min_fps']} | {v['max_fps']} |"
        )
    if summary["notes"]:
        out += ["", "## Notes", ""] + [f"- {n}" for n in summary["notes"]]
    return "\n".join(out) + "\n"


CSS = """
body{background:#0d1117;color:#e6edf3;font:14px/1.45 system-ui,sans-serif;margin:24px}
table{border-collapse:collapse;margin:8px 0 20px}
td,th{border:1px solid #30363d;padding:4px 8px;text-align:left;vertical-align:top}
th{background:#161b22}.pass{color:#3fb950}.fail{color:#f85149}.warn{color:#d29922}code{font-size:12px}
td.v{width:14px;padding:0;height:14px}
.v-pass{background:#238636}.v-pass-wide{background:#9e6a03}.v-fail{background:#da3633}
.v-capture-loss,.v-error,.v-no-stream{background:#6e7681}
"""


def to_html(plan: Plan, summary: dict, verdicts: list) -> str:
    e = html.escape
    ok = summary["passed"]
    parts = [
        f"<!doctype html><meta charset=utf-8><title>MTL stability</title><style>{CSS}</style>"
    ]
    parts.append(
        f"<h1>MTL stability run: <span class={'pass' if ok else 'fail'}>{'PASSED' if ok else 'FAILED'}</span></h1>"
    )
    parts.append(
        f"<p>Profile <code>{e(summary['profile'])}</code>, {summary['ran_s']:.0f} s of {summary['duration_s']} s.</p>"
    )
    if summary["reasons"]:
        parts.append(
            "<h2>Reasons</h2><ul>"
            + "".join(f"<li>{e(r)}</li>" for r in summary["reasons"])
            + "</ul>"
        )
    parts.append(
        "<h2>Streams</h2><table><tr>"
        + "".join(f"<th>{e(h)}</th>" for h in STREAM_HEAD)
        + "</tr>"
    )
    for r in _streams_table(plan, summary):
        parts.append("<tr>" + "".join(f"<td>{e(c)}</td>" for c in r) + "</tr>")
    parts.append("</table>")
    # One cell for each EBU LIST capture, in time order, one row for each stream.
    by_sid: dict = {}
    for v in verdicts:
        d = dataclasses.asdict(v) if dataclasses.is_dataclass(v) else v
        by_sid.setdefault(d["sid"], []).append(d)
    if by_sid:
        parts.append(
            "<h2>EBU LIST verdicts in time order</h2><p>Green pass, amber wide, red fail,"
            " grey capture loss or error. Hover for the reasons.</p><table>"
        )
        for sid, vs in by_sid.items():
            cells = "".join(
                f"<td class='v v-{e(d['verdict'])}' "
                f"title='{e(d['verdict'] + ': ' + '; '.join(d['reasons']))}'></td>"
                for d in vs
            )
            parts.append(f"<tr><th>{e(sid)}</th>{cells}</tr>")
        parts.append("</table>")
    logs = summary["logs"]
    if logs["error_counts"]:
        parts.append(
            "<h2>Log lines of an error class</h2>"
            "<table><tr><th>Worker/class</th><th>Lines</th><th>First lines</th></tr>"
        )
        for k, n in logs["error_counts"].items():
            first = "<br>".join(
                e(f"{t:>7}s {line}")
                for t, line in (logs["error_lines"].get(k) or [])[:5]
            )
            parts.append(
                f"<tr><td>{e(k)}</td><td>{n}</td><td><code>{first}</code></td></tr>"
            )
        parts.append("</table>")
    parts.append(
        "<h2>Session fps</h2><table><tr><th>Session</th><th>Samples</th><th>Median</th><th>Min</th><th>Max</th></tr>"
    )
    for k, v in sorted(logs["sessions"].items()):
        cls = "warn" if v["min_fps"] < v["median_fps"] * 0.98 else ""
        parts.append(
            f"<tr class='{cls}'><td>{e(k)}</td>"
            + "".join(
                f"<td>{v[c]}</td>"
                for c in ("samples", "median_fps", "min_fps", "max_fps")
            )
            + "</tr>"
        )
    parts.append("</table>")
    if summary["notes"]:
        parts.append(
            "<h2>Notes</h2><ul>"
            + "".join(f"<li>{e(n)}</li>" for n in summary["notes"])
            + "</ul>"
        )
    return "\n".join(parts)
