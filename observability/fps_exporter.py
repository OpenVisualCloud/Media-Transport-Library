#!/usr/bin/env python3
"""Dependency-free FFmpeg -progress Prometheus exporter."""
from __future__ import annotations

import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.getenv("METRICS_PORT", "9101"))
PATH = os.getenv("PROGRESS_FILE", "/run/mxl/progress")
ROLE = os.getenv("ROLE", "unknown")
POD = os.getenv("POD", os.getenv("HOSTNAME", "unknown"))


def number(value: str | None) -> float | None:
    if not value or value.upper() == "N/A":
        return None
    token = ""
    for char in value.rstrip("x"):
        if char.isdigit() or char in ".+-eE":
            token += char
        else:
            break
    try:
        return float(token)
    except ValueError:
        return None


def latest() -> dict[str, str]:
    try:
        with open(PATH, "rb") as stream:
            try:
                stream.seek(-16384, os.SEEK_END)
            except OSError:
                pass
            lines = stream.read().decode(errors="replace").splitlines()
    except OSError:
        return {}
    ends = [i for i, line in enumerate(lines) if line.startswith("progress=")]
    if not ends:
        return {}
    start = ends[-2] + 1 if len(ends) > 1 else 0
    return dict(line.split("=", 1) for line in lines[start : ends[-1] + 1] if "=" in line)


def body() -> bytes:
    block = latest()
    labels = f'role="{ROLE}",pod="{POD}"'
    values = {
        "mxl_ffmpeg_fps": number(block.get("fps")),
        "mxl_ffmpeg_speed_ratio": number(block.get("speed")),
        "mxl_ffmpeg_frames_total": number(block.get("frame")),
        "mxl_ffmpeg_drop_frames_total": number(block.get("drop_frames")),
        "mxl_ffmpeg_dup_frames_total": number(block.get("dup_frames")),
        "mxl_ffmpeg_out_time_seconds": (number(block.get("out_time_us")) or 0) / 1_000_000,
        "mxl_ffmpeg_up": 1 if block else 0,
    }
    lines = []
    for metric, value in values.items():
        if value is not None:
            lines.append(f"# TYPE {metric} gauge")
            lines.append(f"{metric}{{{labels}}} {value}")
    return ("\n".join(lines) + "\n").encode()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path != "/metrics":
            self.send_response(404); self.end_headers(); return
        payload = body()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers(); self.wfile.write(payload)

    def log_message(self, *_args):
        return


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
