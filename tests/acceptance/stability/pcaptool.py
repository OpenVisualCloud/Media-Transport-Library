# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Look into a capture of one RTP stream: python -m stability.pcaptool FILE.pcap [--fps p59]

It gives, for a nanosecond pcap of one stream:

* the first packet of each frame against the epoch grid (N x Tframe of TAI);
* the span of each frame and the largest gap between two packets in a frame;
* each break of the RTP sequence number (the extended number for ST 2110-20);
* packet time - RTP time of the first packet of each frame (the RTP time
  unwrapped with the capture time).

It is the evidence for an EBU LIST verdict: a frame that starts late, a stall
inside a frame, or a gap of the capture every 64 packets.
"""

from __future__ import annotations

import argparse
import statistics
import struct
import sys
from fractions import Fraction

FRAME_TIME_NS = {
    "p59": Fraction(10**9 * 1001, 60000),
    "p50": Fraction(10**9, 50),
    "p29": Fraction(10**9 * 1001, 30000),
    "p25": Fraction(10**9, 25),
}
MEDIA_CLOCK = 90000
SMALL_PACKET = 200  # a user-meta packet, not a video packet


def packets(path: str):
    """(time ns, rtp seq, ext seq, rtp ts, marker, wire length) of each packet."""
    with open(path, "rb") as f:
        head = f.read(24)
        nano = struct.unpack("<I", head[:4])[0] == 0xA1B23C4D
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            sec, frac, incl, orig = struct.unpack("<IIII", h)
            d = f.read(incl)
            t = sec * 10**9 + (frac if nano else frac * 1000)
            rtp = 14 + (d[14] & 15) * 4 + 8
            if len(d) < rtp + 14:
                continue
            seq = struct.unpack(">H", d[rtp + 2 : rtp + 4])[0]
            ext = struct.unpack(">H", d[rtp + 12 : rtp + 14])[0]
            ts = struct.unpack(">I", d[rtp + 4 : rtp + 8])[0]
            yield t, seq, ext, ts, d[rtp + 1] >> 7, orig


def rtp_ns(ts: int, t: int) -> float:
    """The RTP time in ns, unwrapped with the capture time t."""
    ticks_now = t * MEDIA_CLOCK // 10**9
    k = (ticks_now - ts) // 2**32
    return (ts + k * 2**32) * 10**9 / MEDIA_CLOCK


def analyze(path: str, fps: str = "p59", video: bool = True) -> dict:
    T = float(FRAME_TIME_NS[fps])
    frames, cur, breaks = [], [], []
    prev = None
    n = 0
    for t, seq, ext, ts, m, length in packets(path):
        n += 1
        if video and length < SMALL_PACKET:
            continue
        full = (ext << 16) | seq if video else seq
        mod = 2**32 if video else 2**16
        if prev is not None and full != (prev + 1) % mod:
            breaks.append({"at_pkt": n, "missing": (full - prev - 1) % mod})
        prev = full
        cur.append((t, ts))
        if m:
            frames.append(cur)
            cur = []
    out: dict = {
        "packets": n,
        "frames": max(0, len(frames) - 1),
        "seq_breaks": len(breaks),
        "first_breaks": breaks[:8],
    }
    if len(breaks) > 2:
        steps = [b["at_pkt"] - a["at_pkt"] for a, b in zip(breaks, breaks[1:])]
        out["break_step_median"] = statistics.median(steps)
    fr = frames[1:]
    if fr:
        off = [(f[0][0] % T) / 1e3 for f in fr]
        span = [(f[-1][0] - f[0][0]) / 1e3 for f in fr]
        gap = [
            max((b[0] - a[0] for a, b in zip(f, f[1:])), default=0) / 1e3 for f in fr
        ]
        prt = [(f[0][0] - rtp_ns(f[0][1], f[0][0])) / 1e3 for f in fr]
        for name, vals in (
            ("first_minus_epoch_us", off),
            ("span_us", span),
            ("max_gap_us", gap),
            ("pkt_minus_rtp_us", prt),
        ):
            out[name] = {
                "min": round(min(vals), 1),
                "median": round(statistics.median(vals), 1),
                "max": round(max(vals), 1),
            }
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="python -m stability.pcaptool", description=__doc__
    )
    ap.add_argument("pcap", nargs="+")
    ap.add_argument("--fps", default="p59", choices=sorted(FRAME_TIME_NS))
    ap.add_argument(
        "--audio", action="store_true", help="no extended sequence number, no frames"
    )
    a = ap.parse_args(argv)
    for p in a.pcap:
        r = analyze(p, a.fps, video=not a.audio)
        print(p)
        for k, v in r.items():
            print(f"  {k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
