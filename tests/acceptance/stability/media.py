# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Make the input files of the workload with FFmpeg, once, into ``media_dir``.

RxTxApp sends each file again from its start when it ends. A short file is
enough, and a RAM disk (``/dev/shm``) keeps file reads out of the TX timing.
The RFC 4175 files come from the FFmpeg ``bitpacked`` encoder (4:2:2 10 bit,
big endian pixel groups, the ST 2110-20 wire layout).
"""

from __future__ import annotations

import logging
import os
import shutil
import subprocess

from .config import HostConfig

logger = logging.getLogger(__name__)

SRC = "testsrc2=size={w}x{h}:rate=60000/1001"

# name -> (lavfi source or None for audio, frames or seconds, codec args)
VIDEO = {
    "2160p.yuv422p10le": (3840, 2160, 8, ["-pix_fmt", "yuv422p10le", "-f", "rawvideo"]),
    "1080p.yuv422p10le": (
        1920,
        1080,
        24,
        ["-pix_fmt", "yuv422p10le", "-f", "rawvideo"],
    ),
    "720p.yuv422p10le": (1280, 720, 24, ["-pix_fmt", "yuv422p10le", "-f", "rawvideo"]),
    "1080p.v210": (
        1920,
        1080,
        24,
        ["-pix_fmt", "yuv422p10le", "-c:v", "v210", "-f", "rawvideo"],
    ),
    "2160p.rfc4175": (
        3840,
        2160,
        8,
        ["-pix_fmt", "yuv422p10le", "-c:v", "bitpacked", "-f", "rawvideo"],
    ),
    "1080p.rfc4175": (
        1920,
        1080,
        24,
        ["-pix_fmt", "yuv422p10le", "-c:v", "bitpacked", "-f", "rawvideo"],
    ),
    "720p.rfc4175": (
        1280,
        720,
        24,
        ["-pix_fmt", "yuv422p10le", "-c:v", "bitpacked", "-f", "rawvideo"],
    ),
}
# name -> (rate, channels, sample format of FFmpeg)
AUDIO = {
    "audio-pcm24-48kHz-2ch.raw": (48000, 2, "s24be"),
    "audio-pcm24-96kHz-2ch.raw": (96000, 2, "s24be"),
    "audio-pcm16-48kHz-8ch.raw": (48000, 8, "s16be"),
}
ANC_TEXT = "MTL stability ST 2110-40 closed caption payload 0123456789\n" * 4


def prepare(host: HostConfig) -> list[str]:
    """Make each file that is missing; return the names it made."""
    os.makedirs(host.media_dir, exist_ok=True)
    ffmpeg = host.ffmpeg or shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("media: no ffmpeg to make the input files")
    made = []
    for name, (w, h, frames, args) in VIDEO.items():
        out = os.path.join(host.media_dir, name)
        if os.path.exists(out) and os.path.getsize(out):
            continue
        cmd = [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            SRC.format(w=w, h=h),
            "-frames:v",
            str(frames),
            *args,
            out,
        ]
        _run(cmd, name)
        made.append(name)
    for name, (rate, ch, fmt) in AUDIO.items():
        out = os.path.join(host.media_dir, name)
        if os.path.exists(out) and os.path.getsize(out):
            continue
        src = f"sine=frequency=997:sample_rate={rate}:duration=10"
        cmd = [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            src,
            "-ac",
            str(ch),
            "-f",
            fmt,
            out,
        ]
        _run(cmd, name)
        made.append(name)
    anc = os.path.join(host.media_dir, "anc.txt")
    if not os.path.exists(anc):
        with open(anc, "w", encoding="ascii") as f:
            f.write(ANC_TEXT)
        made.append("anc.txt")
    return made


def _run(cmd: list[str], name: str) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if r.returncode:
        raise RuntimeError(
            f"media: ffmpeg could not make {name}: {r.stderr.strip()[-500:]}"
        )
    logger.info("media: made %s", name)
