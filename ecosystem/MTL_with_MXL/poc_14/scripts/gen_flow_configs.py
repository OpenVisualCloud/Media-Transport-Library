#!/usr/bin/env python3
"""
Generate 16 per-stream MXL flow definition JSONs from a template.
Each gets a unique UUID while keeping all other parameters identical.

Usage:
    python3 gen_flow_configs.py [--template TEMPLATE] [--outdir OUTDIR] [--count N]
"""

import argparse
import json
import os
import uuid


def main():
    parser = argparse.ArgumentParser(description="Generate per-stream flow configs")
    parser.add_argument(
        "--template",
        default=None,
        help="Template flow JSON (default: ../poc/config/v210_1080p30.json)",
    )
    parser.add_argument(
        "--outdir", default=None, help="Output directory (default: ./config)"
    )
    parser.add_argument(
        "--count", type=int, default=16, help="Number of streams (default: 16)"
    )
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    poc_14_dir = os.path.dirname(script_dir)

    template_path = args.template or os.path.join(
        poc_14_dir, "..", "poc", "config", "v210_1080p30.json"
    )
    out_dir = args.outdir or os.path.join(poc_14_dir, "config")

    os.makedirs(out_dir, exist_ok=True)

    with open(template_path, "r") as f:
        template = json.load(f)

    for i in range(args.count):
        flow = dict(template)
        # Generate a deterministic UUID based on the stream index
        # so re-running the script produces the same IDs
        flow["id"] = str(uuid.uuid5(uuid.NAMESPACE_URL, f"mtl-mxl-poc14-stream-{i}"))
        flow["label"] = f"MTL-MXL 16-Stream Demo, Stream {i}"
        flow["description"] = f"1080p29.97 v210 — stream {i} of {args.count}"

        out_path = os.path.join(out_dir, f"v210_1080p30_{i}.json")
        with open(out_path, "w") as f:
            json.dump(flow, f, indent=2)
            f.write("\n")

        print(f"  [{i:2d}] {out_path}  id={flow['id']}")

    print(f"\nGenerated {args.count} flow configs in {out_dir}")


if __name__ == "__main__":
    main()
