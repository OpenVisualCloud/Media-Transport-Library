# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Read the log of each worker while it runs.

Each scan reads only the new bytes of each log. It records:

* the fps of each MTL session of each stat dump (``TX_VIDEO_SESSION(0,0:..):
  fps 59.94 ...``), so the report can draw it over time and find a stall;
* each line of a known error class (``ERROR_CLASSES``), with a count per class
  and the first lines of each;
* the result lines of RxTxApp at the end (``app_rx_st20p_result(0), OK``).
"""

from __future__ import annotations

import collections
import dataclasses
import os
import re
import statistics
import time

FPS_RE = re.compile(
    r"\b((?:TX|RX)_[A-Z0-9_]+?)\(([^)]*)\)[^\n]*?\bfps\s+([0-9]+(?:\.[0-9]+)?)", re.I
)
RESULT_RE = re.compile(
    r"\b(app_(?:tx|rx)_[a-z0-9]+_result)\(([^)]*)\)[,:]?\s*(OK|FAILED|FAIL)", re.I
)
RESYNC_RE = re.compile(r"\b(app_tx_[a-z0-9]+)_uinit\((\d+)\), user time resync (\d+)")
# One resync at start is normal: RxTxApp stamps its first frames before mtl_start.
RESYNC_EXPECTED = 1
COUNTER_RE = re.compile(
    r"\b((?:TX|RX)_[A-Z0-9_]+?)\(([^)]*)\):\s*([a-z][a-z _-]+?)\s+(?:pkts\s+)?(\d+)\b"
)

# A counter of a session stat line that must stay at 0 in a stable run.
BAD_COUNTERS = re.compile(
    r"incomplete|dropped|drop|out of order|ooo|late|timeout|fail|error|lost|loss|mismatch|retransmit|redundant",
    re.I,
)

# (class, regex). The first class that matches a line takes it.
ERROR_CLASSES = [
    (
        "crash",
        re.compile(
            r"segmentation fault|core dumped|SIGSEGV|SIGABRT|Aborted|assert(ion)? fail|stack smashing|double free",
            re.I,
        ),
    ),
    (
        "sha-mismatch",
        re.compile(r"sha\s*(check|result)?\s*(fail|mismatch|error)", re.I),
    ),
    (
        "tx-late",
        re.compile(
            r"\b(late|epoch.*(drop|skip|mismatch)|user timestamp|tx_late|frame late|missed)\b",
            re.I,
        ),
    ),
    (
        "build-timeout",
        re.compile(r"build timeout|tx timeout|timeout.*(frame|pkt)", re.I),
    ),
    ("rx-incomplete", re.compile(r"incomplete frame", re.I)),
    ("ptp", re.compile(r"\bptp\b.*(fail|error|lost|timeout)", re.I)),
    (
        "ffmpeg-error",
        re.compile(r"\[(mtl_st[0-9a-z_]+|libsvtjpegxs)[^\]]*\] .*(error|fail)", re.I),
    ),
    ("gst-error", re.compile(r"^ERROR: |GStreamer-CRITICAL|GStreamer-WARNING")),
    ("mtl-error", re.compile(r"(^|\s)(Error|ERR)[:,]|\berr\(|\bfail(ed)?\b", re.I)),
]
# Lines that match a class but are normal at start or stop.
BENIGN = re.compile(
    r"(ptp.*not (enabled|supported)|failed to set.*cpufreq|eal.*(no free|no available) hugepages reported|"
    r"init_retry|retry|tsc_hz.*fail|kni|vfio.*noiommu|mtl_init.*retry|EAL: (VFIO|Probe|Requested)|"
    r"st_app_.*_result\(\d+\), OK|app_.*_result.*OK|arp|dhcp|not support(ed)? (tm|rl).*try|"
    r"fallback to tsc|sch_.*quota|warn\(\"|fail to set thread name)",
    re.I,
)
WARMUP_S = 30.0
STALL_RATIO = 0.98  # an fps sample below 98 % of the median of its session


@dataclasses.dataclass
class SessionTrack:
    samples: list = dataclasses.field(default_factory=list)  # (t, fps)

    def median(self) -> float:
        vals = [f for _, f in self.samples]
        return statistics.median(vals) if vals else 0.0


class LogScanner:
    def __init__(self, workers: list[str], log_path, max_lines: int = 200):
        self.log_path = log_path  # name -> path
        self.offsets = {w: 0 for w in workers}
        self.partial = {w: "" for w in workers}
        self.start = time.time()
        self.max_lines = max_lines
        self.tracks: dict = collections.defaultdict(
            SessionTrack
        )  # (worker, label) -> track
        self.counts: dict = collections.Counter()  # (worker, class) -> n
        self.lines: dict = collections.defaultdict(
            list
        )  # (worker, class) -> [(t, line)]
        self.results: dict = {}  # (worker, label) -> OK | FAILED
        self.counters: dict = {}  # (worker, label, name) -> last value
        self.anomalies: list = []  # (t, worker, text)
        self.resyncs: dict = {}  # (worker, session) -> count

    def _classify(self, line: str):
        if BENIGN.search(line):
            return None
        for name, rx in ERROR_CLASSES:
            if rx.search(line):
                return name
        return None

    def scan(self) -> int:
        """Read the new lines of each log; return how many lines it read."""
        n = 0
        now = time.time()
        for w, off in self.offsets.items():
            path = self.log_path(w)
            if not os.path.exists(path):
                continue
            with open(path, "rb") as f:
                f.seek(off)
                data = f.read()
                self.offsets[w] = off + len(data)
            text = self.partial[w] + data.decode("utf-8", "replace")
            lines = text.split("\n")
            self.partial[w] = lines.pop()
            for line in lines:
                n += 1
                self._line(w, line, now)
        return n

    def _line(self, w: str, line: str, now: float) -> None:
        m = FPS_RE.search(line)
        if m:
            key = (w, f"{m.group(1)}({m.group(2)})")
            fps = float(m.group(3))
            track = self.tracks[key]
            track.samples.append((now - self.start, fps))
            if now - self.start > WARMUP_S and len(track.samples) > 3:
                med = track.median()
                if med > 0 and fps < med * STALL_RATIO:
                    self.anomalies.append(
                        (now, w, f"{key[1]} fps {fps:.3f}, median {med:.3f}")
                    )
            return
        m = RESYNC_RE.search(line)
        if m:
            self.resyncs[(w, f"{m.group(1)}({m.group(2)})")] = int(m.group(3))
            return
        m = RESULT_RE.search(line)
        if m:
            self.results[(w, f"{m.group(1)}({m.group(2)})")] = (
                "OK" if m.group(3).upper() == "OK" else "FAILED"
            )
            if m.group(3).upper() != "OK":
                self._record(w, "result-failed", line, now)
            return
        m = COUNTER_RE.search(line)
        if m and BAD_COUNTERS.search(m.group(3)) and int(m.group(4)) > 0:
            key = (w, f"{m.group(1)}({m.group(2)})", m.group(3).strip())
            self.counters[key] = self.counters.get(key, 0) + int(m.group(4))
            if now - self.start > WARMUP_S:
                self._record(w, "counter", line, now)
            return
        cls = self._classify(line)
        if cls:
            self._record(w, cls, line, now)

    def _record(self, w: str, cls: str, line: str, now: float) -> None:
        self.counts[(w, cls)] += 1
        kept = self.lines[(w, cls)]
        if len(kept) < self.max_lines:
            kept.append((round(now - self.start, 1), line.strip()[:400]))

    def summary(self) -> dict:
        sessions = {}
        for (w, label), t in self.tracks.items():
            vals = [f for _, f in t.samples]
            sessions[f"{w}/{label}"] = {
                "samples": len(vals),
                "median_fps": round(t.median(), 3),
                "min_fps": round(min(vals), 3) if vals else 0,
                "max_fps": round(max(vals), 3) if vals else 0,
            }
        return {
            "sessions": sessions,
            "error_counts": {
                f"{w}/{c}": n for (w, c), n in sorted(self.counts.items())
            },
            "error_lines": {f"{w}/{c}": v for (w, c), v in sorted(self.lines.items())},
            "bad_counters": {
                f"{w}/{label}/{name}": v
                for (w, label, name), v in sorted(self.counters.items())
            },
            "resyncs": {f"{w}/{k}": n for (w, k), n in sorted(self.resyncs.items())},
            "results": {
                f"{w}/{label}": r for (w, label), r in sorted(self.results.items())
            },
            "anomalies": [
                {"t": round(t - self.start, 1), "worker": w, "text": x}
                for t, w, x in self.anomalies[: self.max_lines]
            ],
        }

    def timeline(self) -> dict:
        return {f"{w}/{label}": t.samples for (w, label), t in self.tracks.items()}
