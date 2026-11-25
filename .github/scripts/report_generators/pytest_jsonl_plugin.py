# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Pytest plugin emitting structured results.jsonl matching gtest_runner.py.

Activated by setting MTL_PYTEST_JSONL=<path> and passing `-p pytest_jsonl_plugin`.
"""

from __future__ import annotations

import json
import os
import socket
from pathlib import Path
from typing import Any

_RECORDS: list[dict[str, Any]] = []


def _out_path() -> Path | None:
    raw = os.environ.get("MTL_PYTEST_JSONL", "").strip()
    return Path(raw) if raw else None


def _classify(report: Any) -> str | None:
    if hasattr(report, "wasxfail"):
        if report.skipped:
            return "xfailed"
        if report.passed:
            return "xpassed"
    if report.when == "call":
        return str(report.outcome)
    if report.when in ("setup", "teardown"):
        if report.failed:
            return "error"
        if report.skipped and report.when == "setup":
            return "skipped"
    return None


def pytest_runtest_logreport(report: Any) -> None:
    if _out_path() is None:
        return
    status = _classify(report)
    if status is None:
        return
    msg = ""
    if status in ("failed", "error", "skipped", "xfailed"):
        msg = str(
            getattr(report, "longreprtext", "") or getattr(report, "longrepr", "")
        )[:2000]
    _RECORDS.append(
        {
            "case": str(report.nodeid),
            "category": os.environ.get("TEST_PATH", "pytest"),
            "nic": os.environ.get(
                "MTL_NIC_LABEL", os.environ.get("NIC", "UNKNOWN")
            ).upper(),
            "host": socket.gethostname(),
            "platform": "pytest",
            "result": status,
            "duration_ms": round(
                float(getattr(report, "duration", 0.0) or 0.0) * 1000.0, 1
            ),
            "message": msg,
        }
    )


def pytest_sessionfinish(session: Any, exitstatus: int) -> None:
    out = _out_path()
    if out is None:
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as fh:
        for rec in _RECORDS:
            fh.write(json.dumps(rec) + "\n")
    _RECORDS.clear()
