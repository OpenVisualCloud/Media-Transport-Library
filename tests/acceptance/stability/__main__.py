# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""python -m stability {plan|run|unfence|media} -c stability.yaml ...

Run it from tests/acceptance with the acceptance venv, as root (``sudo -E``).
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import time

from . import config, media
from .fence import restore
from .runner import Run
from .workload import PROFILES, build_plan


def _duration(text: str) -> int:
    """'90', '90s', '15m', '8h' -> seconds."""
    text = text.strip().lower()
    mult = {"s": 1, "m": 60, "h": 3600}.get(text[-1:], None)
    return int(float(text[:-1]) * mult) if mult else int(text)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m stability", description=__doc__)
    ap.add_argument("command", choices=["plan", "run", "unfence", "media", "check"])
    ap.add_argument(
        "-c",
        "--config",
        default=os.environ.get("MTL_STABILITY_CONFIG", "stability.yaml"),
    )
    ap.add_argument("-p", "--profile", default="full", choices=sorted(PROFILES))
    ap.add_argument("-d", "--duration", default="15m", help="90s, 15m, 8h")
    ap.add_argument(
        "-o",
        "--out",
        default="",
        help="run directory (default: <run_dir>/<time>-<profile>)",
    )
    ap.add_argument("--only", default="", help="comma-separated worker names")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if a.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    if a.command == "unfence":
        print("\n".join(restore()))
        return 0
    host = config.load(a.config)
    if a.command == "check":
        problems = host.check()
        print("\n".join(problems) or "host config: OK")
        return 1 if problems else 0
    if a.command == "media":
        print("\n".join(media.prepare(host)) or "media: each file is there")
        return 0
    plan = build_plan(
        host, a.profile, _duration(a.duration), [w for w in a.only.split(",") if w]
    )
    if a.command == "plan":
        print(plan.to_json())
        return 0
    out = a.out or os.path.join(
        host.run_dir, time.strftime("%Y%m%d-%H%M%S") + f"-{plan.profile}"
    )
    summary = Run(host, plan, out).execute()
    print(
        json.dumps(
            {k: summary[k] for k in ("passed", "reasons", "ebu", "ebu_never_captured")},
            indent=2,
        )
    )
    print(f"report: {os.path.join(out, 'report.html')}")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
