#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Regenerate the examples page from examples.md.in.

The template holds the prose. Each @@EXAMPLE <file>@@ marker becomes the example
file copied verbatim (check.sh lint 4 checks it), and @@HEADER_TABLE@@ becomes the
table of headers and their jobs (the counts are check.sh's output, not the page's).
Run it after changing an example, a header or the template.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "examples.md")
HDR = os.path.join(HERE, "include", "mtl", "experimental")
EXAMPLES = os.path.join(HERE, "examples")

JOBS = [
    (
        "mtl.h",
        "instance, session config and lifecycle, the unit and the frame-unit layout of every "
        "essence (the ANC packet table too), TX and RX verbs, results, waiting, errors; the "
        "object verbs (close, interrupt, wait, reap) and the lease release; queues for event "
        "loops (MS2)",
    ),
    (
        "mtl_mem.h",
        "regions, attached pools, named slots; the requirements struct",
    ),
    (
        "mtl_sync.h",
        "epoch index arithmetic, media clock ticks, the next-unit record, row deadlines and RX row "
        "waits, A/V alignment, clocks, the time reference",
    ),
    (
        "mtl_events.h",
        "the events of a session and of an instance (the inline notify: `MTL_LATER`)",
    ),
    (
        "mtl_packet.h",
        "packet tables, RTP and payload header layouts and the RFC 8331 codec for "
        "`MTL_UNIT_PACKETS`",
    ),
    (
        "mtl_observe.h",
        "a service's operations side: stats registry, full results, RX detail and timing, "
        "enumeration, ports, logging, capture, health, shutdown",
    ),
    ("mtl_options.h", "option keys and their value enums; set, reset, get, list, find"),
    ("mtl_reasons.h", "the reason codes"),
    (
        "mtl_format.h",
        "application formats, colorimetry, format descriptions; "
        "standalone colour and audio conversion",
    ),
    (
        "mtl_util.h",
        "inline helpers (it includes `mtl_mem.h` and `mtl_sync.h`): copy path, one-call slot "
        "send, unit and plane copies, meta records, ANC tables and words, the RX reserve",
    ),
    ("mtl_plugin.h", "codec and converter plugin ABI v2"),
    ("mtl_legacy.h", "bridge from a legacy `mtl_handle`; legacy enum values as unified ones"),
    ("mtl_debug.h", "fault injection and the test clock (debug builds)"),
    (
        "mtl_ipmx.h",
        "RTCP sender reports and the Info Block, payload encryption "
        "(Phase 7, under `MTL_LATER`)",
    ),
    (
        "mtl_sdp.h",
        "SDP render and parse, in the companion library libmtl_sdp "
        "(Phase 7, under `MTL_LATER`)",
    ),
]


def header_table():
    missing = sorted(
        set(f for f in os.listdir(HDR) if f.endswith(".h")) - set(n for n, _ in JOBS)
    )
    if missing:
        sys.exit("gen_api_doc.py has no job for: " + ", ".join(missing))
    rows = ["| Header | Job |", "|---|---|"]
    for name, job in JOBS:
        link = "sketch/include/mtl/experimental/" + name
        rows.append("| [`%s`](%s) | %s |" % (name, link, job))
    rows.append("")
    rows.append(
        "[`sketch/check.sh`](sketch/check.sh) prints the exported functions per header, "
        "per call class and per milestone, and the number of frozen names."
    )
    return "\n".join(rows)


def example_block(match):
    name = match.group(1)
    lang = "cpp" if name.endswith(".cpp") else "c"
    with open(os.path.join(EXAMPLES, name)) as f:
        return "```%s\n%s\n```" % (lang, f.read().rstrip("\n"))


def main():
    with open(os.path.join(HERE, "examples.md.in")) as f:
        text = f.read()
    missing = [
        f for f in sorted(os.listdir(EXAMPLES)) if "@@EXAMPLE %s@@" % f not in text
    ]
    if missing:
        sys.exit("examples.md.in does not show: " + ", ".join(missing))
    text = text.replace("@@HEADER_TABLE@@", header_table())
    text = re.sub(r"@@EXAMPLE ([A-Za-z0-9_.]+)@@", example_block, text)
    with open(OUT, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
