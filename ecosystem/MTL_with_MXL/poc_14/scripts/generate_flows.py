#!/usr/bin/env python3
"""
Generate per-stream MXL flow JSON files from the main config.

Reads streams_14.json (or streams_14_rx.json), extracts global video
params (resolution, fps), and writes one flow JSON per active stream.

This means you only change fps/resolution in ONE place (the config's
global.video section) and this script propagates it to all flow files.

Usage:
    python3 generate_flows.py <config.json> [--output-dir <dir>]

The generated files are named:  <output-dir>/flow_<stream_id>.json
"""

import argparse
import json
import os
import sys
import uuid


def generate_flow_json(stream_id: int, label: str, width: int, height: int,
                       fps_num: int, fps_den: int) -> dict:
    """Build an MXL flow descriptor for a single stream."""
    # Deterministic UUID based on stream id (reproducible, no random jitter)
    flow_id = str(uuid.uuid5(uuid.NAMESPACE_DNS,
                              f"mtl-mxl-poc14-stream-{stream_id}"))

    return {
        "$copyright": "SPDX-FileCopyrightText: 2025 MTL-MXL POC",
        "$license": "SPDX-License-Identifier: BSD-3-Clause",
        "description": f"{width}x{height} v210 — stream {stream_id}",
        "id": flow_id,
        "tags": {
            "urn:x-nmos:tag:grouphint/v1.0": ["MTL-MXL POC:Video"]
        },
        "format": "urn:x-nmos:format:video",
        "label": label or f"MTL-MXL Stream {stream_id}",
        "parents": [],
        "media_type": "video/v210",
        "grain_rate": {
            "numerator": fps_num,
            "denominator": fps_den
        },
        "frame_width": width,
        "frame_height": height,
        "interlace_mode": "progressive",
        "colorspace": "BT709",
        "components": [
            {"name": "Y",  "width": width,     "height": height, "bit_depth": 10},
            {"name": "Cb", "width": width // 2, "height": height, "bit_depth": 10},
            {"name": "Cr", "width": width // 2, "height": height, "bit_depth": 10},
        ]
    }


def main():
    parser = argparse.ArgumentParser(description="Generate per-stream flow JSONs")
    parser.add_argument("config", help="Path to streams_14.json or streams_14_rx.json")
    parser.add_argument("--output-dir", default=None,
                        help="Directory for flow_*.json (default: same dir as config)")
    args = parser.parse_args()

    with open(args.config) as f:
        cfg = json.load(f)

    g = cfg.get("global", {})
    video = g.get("video", {})
    width   = video.get("width", 1920)
    height  = video.get("height", 1080)
    fps_num = video.get("fps_num", 30000)
    fps_den = video.get("fps_den", 1001)

    streams = cfg.get("streams", [])
    active  = g.get("active_streams", len(streams))
    active  = min(active, len(streams))

    out_dir = args.output_dir or os.path.dirname(os.path.abspath(args.config))
    os.makedirs(out_dir, exist_ok=True)

    for s in streams[:active]:
        sid   = s.get("id", streams.index(s))
        label = s.get("label", f"Stream {sid + 1}")
        flow  = generate_flow_json(sid, label, width, height, fps_num, fps_den)

        path = os.path.join(out_dir, f"flow_{sid}.json")
        with open(path, "w") as f:
            json.dump(flow, f, indent=2)
            f.write("\n")

    fps_str = f"{fps_num}/{fps_den}"
    print(f"[generate_flows] {active} flow files → {out_dir}/  "
          f"({width}x{height} @ {fps_str})")


if __name__ == "__main__":
    main()
