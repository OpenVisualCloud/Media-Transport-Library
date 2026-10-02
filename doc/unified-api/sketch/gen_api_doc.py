#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Regenerate the examples page from examples.md.in.

The template holds the prose. Each @@EXAMPLE <file>@@ marker becomes the example
file copied verbatim (check.sh lint 4 checks it), and @@HEADER_TABLE@@ becomes the
table of headers with their function counts, read from the headers themselves.
Run it after changing an example or a header.
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
        "instance, session config and lifecycle, the unit, TX and RX verbs, "
        "results, waiting, errors",
    ),
    (
        "mtl_mem.h",
        "regions, attached pools, named slots, IOVA; the requirements struct",
    ),
    (
        "mtl_sync.h",
        "timelines, index arithmetic, slot hint, RX row waits, clocks, "
        "the time reference",
    ),
    ("mtl_queue.h", "events, shared queues, dispatcher"),
    (
        "mtl_packet.h",
        "packet tables, RTP and payload header layouts for " "`MTL_UNIT_PACKETS`",
    ),
    (
        "mtl_observe.h",
        "stats registry, full results, RX detail and timing, "
        "enumeration, ports, logging, capture, health, shutdown",
    ),
    ("mtl_options.h", "option keys and their value enums; set, reset, get, list, find"),
    ("mtl_reasons.h", "the reason codes"),
    (
        "mtl_format.h",
        "application pixel formats, colorimetry, names, sizes, pixel "
        "groups, codecs, bandwidth",
    ),
    (
        "mtl_util.h",
        "copy path, one-call slot send, unit copies, ANC and RFC 8331 " "helpers",
    ),
    ("mtl_plugin.h", "codec and converter plugin ABI v2"),
    ("mtl_convert.h", "standalone colour and AM824 conversion"),
    ("mtl_legacy.h", "bridge to a legacy `mtl_handle`"),
    ("mtl_debug.h", "test clock, fault injection (debug builds)"),
    ("mtl_sdp.h", "SDP render and parse (Phase 7: NMOS)"),
    ("mtl_rtcp.h", "RTCP sender reports and the IPMX Info Block (Phase 7: IPMX)"),
    ("mtl_crypto.h", "IPMX payload encryption (Phase 7: IPMX)"),
]


def count(path):
    """Functions declared outside and inside the MTL_LATER blocks."""
    now = later = 0
    in_later = False
    with open(path) as f:
        for line in f:
            if line.startswith("#if defined(MTL_LATER)"):
                in_later = True
            elif in_later and line.startswith("#endif"):
                in_later = False
            elif re.match(r"^MTL_API_(CP|DP|DPC|WT|AS) ", line):
                if in_later:
                    later += 1
                else:
                    now += 1
    return now, later


def header_table():
    rows = ["| Header | Job | Functions |", "|---|---|---|"]
    total_now = total_later = 0
    for name, job in JOBS:
        now, later = count(os.path.join(HDR, name))
        total_now += now
        total_later += later
        n = "inline only" if name == "mtl_packet.h" else str(now)
        if later:
            n += " (+%d later)" % later
        link = "sketch/include/mtl/experimental/" + name
        rows.append("| [`%s`](%s) | %s | %s |" % (name, link, job, n))
    rows.append("")
    rows.append(
        "%d exported functions in total, %d more reserved for later phases "
        "(`MTL_LATER`)." % (total_now, total_later)
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
