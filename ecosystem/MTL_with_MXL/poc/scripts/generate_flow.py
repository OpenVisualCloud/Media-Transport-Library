#!/usr/bin/env python3
"""
Generate the MXL flow JSON for the POC from the master config.

Reads poc.json, extracts video params (resolution, fps), and writes
a single flow_poc.json file.  Change resolution/fps in ONE place
(poc.json) and this script propagates it to the flow file.

Usage:
    python3 generate_flow.py <poc.json> [--output-dir <dir>]

Output: <output-dir>/flow_poc.json
"""

import argparse
import json
import os
import sys
import uuid


def generate_flow_json(width: int, height: int,
                       fps_num: int, fps_den: int) -> dict:
    """Build an MXL flow descriptor for the single-stream POC."""
    flow_id = str(uuid.uuid5(uuid.NAMESPACE_DNS, "mtl-mxl-poc-single-stream"))

    fps_approx = fps_num / fps_den
    res_label = f"{height}p{fps_approx:.2f}".rstrip("0").rstrip(".")
    colorspace = "BT2020" if width > 1920 else "BT709"

    return {
        "$copyright": "SPDX-FileCopyrightText: 2025 MTL-MXL POC",
        "$license": "SPDX-License-Identifier: BSD-3-Clause",
        "description": f"MTL-MXL POC Flow, {res_label} v210",
        "id": flow_id,
        "tags": {
            "urn:x-nmos:tag:grouphint/v1.0": ["MTL-MXL POC:Video"]
        },
        "format": "urn:x-nmos:format:video",
        "label": f"MTL-MXL POC Flow, {res_label}",
        "parents": [],
        "media_type": "video/v210",
        "grain_rate": {
            "numerator": fps_num,
            "denominator": fps_den
        },
        "frame_width": width,
        "frame_height": height,
        "interlace_mode": "progressive",
        "colorspace": colorspace,
        "components": [
            {"name": "Y",  "width": width,     "height": height, "bit_depth": 10},
            {"name": "Cb", "width": width // 2, "height": height, "bit_depth": 10},
            {"name": "Cr", "width": width // 2, "height": height, "bit_depth": 10},
        ]
    }


def main():
    parser = argparse.ArgumentParser(description="Generate POC flow JSON")
    parser.add_argument("config", help="Path to poc.json")
    parser.add_argument("--output-dir", default=None,
                        help="Directory for flow_poc.json (default: same dir as config)")
    args = parser.parse_args()

    with open(args.config) as f:
        cfg = json.load(f)

    video = cfg.get("video", {})
    width   = video.get("width", 1920)
    height  = video.get("height", 1080)
    fps_num = video.get("fps_num", 30000)
    fps_den = video.get("fps_den", 1001)

    out_dir = args.output_dir or os.path.dirname(os.path.abspath(args.config))
    os.makedirs(out_dir, exist_ok=True)

    flow = generate_flow_json(width, height, fps_num, fps_den)
    path = os.path.join(out_dir, "flow_poc.json")
    with open(path, "w") as f:
        json.dump(flow, f, indent=2)
        f.write("\n")

    fps_str = f"{fps_num}/{fps_den}"
    print(f"[generate_flow] → {path}  ({width}x{height} @ {fps_str})")


if __name__ == "__main__":
    main()
