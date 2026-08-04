#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Manually generate synthetic transport-format diagnostic assets.

Frames contain bars, line gradients, motion, and frame-index markers so
transport corruption remains visible to integrity checks.

This tool is not called by pytest or host setup. CI uses real footage from
NFS; generated files are for explicit local diagnostics only.

Usage:
    gen_acceptance_assets.py --out /tmp/mtl_media [--dry-run] [--only NAME]
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mtl_engine import media_files as M  # noqa: E402

DEFAULT_FRAMES = 30


def _base_frame(width: int, height: int, idx: int) -> tuple:
    """Y, Cb, Cr planes at full resolution, 12-bit, as float arrays."""
    x = np.arange(width, dtype=np.float32)
    y = np.arange(height, dtype=np.float32)
    xx, yy = np.meshgrid(x, y)

    # Eight colour bars: distinct luma and chroma per bar so a byte-order or
    # pgroup-packing error shows up as a colour shift rather than as noise.
    bar = np.floor(xx / max(1, width / 8)).astype(np.int32) % 8
    luma_steps = np.array([235, 210, 180, 150, 120, 90, 60, 16], dtype=np.float32)
    cb_steps = np.array([128, 160, 96, 200, 56, 176, 80, 128], dtype=np.float32)
    cr_steps = np.array([128, 96, 176, 64, 192, 80, 160, 128], dtype=np.float32)

    luma = luma_steps[bar]
    cb = cb_steps[bar]
    cr = cr_steps[bar]

    # Vertical gradient: a dropped or swapped line breaks the ramp.
    luma = luma + (yy / max(1, height)) * 16.0

    # Moving box, wrapping once across the clip.
    box_w = max(16, width // 12)
    box_h = max(16, height // 12)
    box_x = int((idx / DEFAULT_FRAMES) * (width - box_w)) % max(1, width - box_w)
    box_y = height // 3
    sel = (xx >= box_x) & (xx < box_x + box_w) & (yy >= box_y) & (yy < box_y + box_h)
    luma = np.where(sel, 940.0 / 4, luma)
    cb = np.where(sel, 512.0 / 4, cb)
    cr = np.where(sel, 512.0 / 4, cr)

    # Frame-index stripe along the top: duplicated frames become visible.
    stripe = max(2, height // 64)
    luma[:stripe, :] = 16.0 + (idx % 16) * 13.0

    # 8-bit reference values scaled to 12-bit working depth.
    return luma * 16.0, cb * 16.0, cr * 16.0


def _quantize(plane: np.ndarray, depth: int) -> np.ndarray:
    shift = 12 - depth
    out = (
        np.right_shift(plane.astype(np.int32), shift)
        if shift
        else plane.astype(np.int32)
    )
    return np.clip(out, 0, (1 << depth) - 1).astype(np.uint16)


def _sub422(plane: np.ndarray) -> np.ndarray:
    return plane[:, 0::2]


def _sub420(plane: np.ndarray) -> np.ndarray:
    return plane[0::2, 0::2]


def _rgb_planes(width: int, height: int, idx: int) -> tuple:
    """R, G, B planes at 12-bit depth, converted from limited-range BT.709."""
    luma, cb, cr = _base_frame(width, height, idx)
    y = luma / 16.0 - 16.0
    u = cb / 16.0 - 128.0
    v = cr / 16.0 - 128.0
    red = 1.164 * y + 1.793 * v
    green = 1.164 * y - 0.213 * u - 0.533 * v
    blue = 1.164 * y + 2.112 * u
    return tuple(np.clip(plane, 0, 255) * 16.0 for plane in (red, green, blue))


def _planar(handle, width, height, idx, depth, sampling, order_rgb=False):
    if order_rgb:
        red, green, blue = _rgb_planes(width, height, idx)
        planes = [green, blue, red]
    else:
        luma, cb, cr = _base_frame(width, height, idx)
        if sampling == "444":
            planes = [luma, cb, cr]
        elif sampling == "422":
            planes = [luma, _sub422(cb), _sub422(cr)]
        else:
            planes = [luma, _sub420(cb), _sub420(cr)]
    for p in planes:
        handle.write(_quantize(p, depth).astype("<u2").tobytes())


def _planar8(handle, width, height, idx, sampling):
    luma, cb, cr = _base_frame(width, height, idx)
    if sampling == "420":
        planes = [luma, _sub420(cb), _sub420(cr)]
    else:
        planes = [luma, _sub422(cb), _sub422(cr)]
    for p in planes:
        handle.write(_quantize(p, 8).astype(np.uint8).tobytes())


def _rfc4175_422_10(handle, width, height, idx):
    """RFC 4175 4:2:2 10-bit: 2 pixels per 5-byte pgroup, Cb Y0 Cr Y1, big-endian."""
    luma, cb, cr = _base_frame(width, height, idx)
    y = _quantize(luma, 10).astype(np.uint64)
    u = _quantize(_sub422(cb), 10).astype(np.uint64)
    v = _quantize(_sub422(cr), 10).astype(np.uint64)

    y0 = y[:, 0::2]
    y1 = y[:, 1::2]
    word = (u << 30) | (y0 << 20) | (v << 10) | y1
    b = np.empty((word.shape[0], word.shape[1], 5), dtype=np.uint8)
    for i in range(5):
        b[:, :, i] = ((word >> (32 - 8 * i)) & 0xFF).astype(np.uint8)
    handle.write(b.tobytes())


def _y210(handle, width, height, idx):
    """Packed 4:2:2, 10-bit left-justified in 16-bit words: Y0 Cb Y1 Cr."""
    luma, cb, cr = _base_frame(width, height, idx)
    y = _quantize(luma, 10) << 6
    u = _quantize(_sub422(cb), 10) << 6
    v = _quantize(_sub422(cr), 10) << 6
    out = np.empty((height, width // 2, 4), dtype=np.uint16)
    out[:, :, 0] = y[:, 0::2]
    out[:, :, 1] = u
    out[:, :, 2] = y[:, 1::2]
    out[:, :, 3] = v
    handle.write(out.astype("<u2").tobytes())


def _uyvy(handle, width, height, idx):
    luma, cb, cr = _base_frame(width, height, idx)
    y = _quantize(luma, 8).astype(np.uint8)
    u = _quantize(_sub422(cb), 8).astype(np.uint8)
    v = _quantize(_sub422(cr), 8).astype(np.uint8)
    out = np.empty((height, width // 2, 4), dtype=np.uint8)
    out[:, :, 0] = u
    out[:, :, 1] = y[:, 0::2]
    out[:, :, 2] = v
    out[:, :, 3] = y[:, 1::2]
    handle.write(out.tobytes())


def _rgb8(handle, width, height, idx):
    """RFC 4175 RGB 8-bit: one pixel per 3-octet pgroup, R G B."""
    red, green, blue = _rgb_planes(width, height, idx)
    out = np.empty((height, width, 3), dtype=np.uint8)
    out[:, :, 0] = _quantize(red, 8).astype(np.uint8)
    out[:, :, 1] = _quantize(green, 8).astype(np.uint8)
    out[:, :, 2] = _quantize(blue, 8).astype(np.uint8)
    handle.write(out.tobytes())


def _v210(handle, width, height, idx):
    """6 pixels per four 32-bit words, three 10-bit components each."""
    if width % 6:
        raise ValueError(f"v210 needs a width divisible by 6, got {width}")
    luma, cb, cr = _base_frame(width, height, idx)
    y = _quantize(luma, 10).astype(np.uint32)
    u = _quantize(_sub422(cb), 10).astype(np.uint32)
    v = _quantize(_sub422(cr), 10).astype(np.uint32)

    groups = width // 6
    y = y.reshape(height, groups, 6)
    u = u.reshape(height, groups, 3)
    v = v.reshape(height, groups, 3)

    w = np.empty((height, groups, 4), dtype=np.uint32)
    w[:, :, 0] = u[:, :, 0] | (y[:, :, 0] << 10) | (v[:, :, 0] << 20)
    w[:, :, 1] = y[:, :, 1] | (u[:, :, 1] << 10) | (y[:, :, 2] << 20)
    w[:, :, 2] = v[:, :, 1] | (y[:, :, 3] << 10) | (u[:, :, 2] << 20)
    w[:, :, 3] = y[:, :, 4] | (v[:, :, 2] << 10) | (y[:, :, 5] << 20)
    handle.write(w.astype("<u4").tobytes())


WRITERS = {
    "YUV422RFC4175PG2BE10": (_rfc4175_422_10, 2.5),
    "YUV422PLANAR10LE": (lambda h, w, ht, i: _planar(h, w, ht, i, 10, "422"), 4),
    "I422_10LE": (lambda h, w, ht, i: _planar(h, w, ht, i, 10, "422"), 4),
    "YUV422PLANAR12LE": (lambda h, w, ht, i: _planar(h, w, ht, i, 12, "422"), 4),
    "YUV444PLANAR10LE": (lambda h, w, ht, i: _planar(h, w, ht, i, 10, "444"), 6),
    "YUV444PLANAR12LE": (lambda h, w, ht, i: _planar(h, w, ht, i, 12, "444"), 6),
    "GBRPLANAR10LE": (
        lambda h, w, ht, i: _planar(h, w, ht, i, 10, "444", order_rgb=True),
        6,
    ),
    "GBRPLANAR12LE": (
        lambda h, w, ht, i: _planar(h, w, ht, i, 12, "444", order_rgb=True),
        6,
    ),
    "YUV420CUSTOM8": (lambda h, w, ht, i: _planar8(h, w, ht, i, "420"), 1.5),
    "Y210": (_y210, 4),
    "UYVY": (_uyvy, 2),
    "RGB8": (_rgb8, 3),
    "v210": (_v210, 8 / 3),
}


def _collect() -> dict:
    """Return transport-input assets supported by the manual generator."""
    wanted = {}
    for entry in M.yuv_files_input_formats.values():
        wanted[entry["filename"]] = {
            "width": entry["width"],
            "height": entry["height"],
            "format": entry["file_format"],
        }
    return wanted


def _write_video(path: Path, spec, frames: int, force: bool) -> bool:
    writer = WRITERS[spec["format"]][0]
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as handle:
            temporary_path = Path(handle.name)
            for idx in range(frames):
                writer(handle, spec["width"], spec["height"], idx)

        bytes_per_frame = int(
            spec["width"] * spec["height"] * WRITERS[spec["format"]][1]
        )
        actual_size = temporary_path.stat().st_size
        expected_size = bytes_per_frame * frames
        if actual_size != expected_size:
            raise RuntimeError(
                f"generated {path.name} has {actual_size} bytes, expected {expected_size}"
            )
        if force:
            temporary_path.replace(path)
        else:
            os.link(temporary_path, path)
        return True
    except FileExistsError:
        return False
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/mtl_media")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", default=None)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    out = Path(args.out)
    wanted = _collect()
    if args.only:
        wanted = {k: v for k, v in wanted.items() if args.only in k}
        if not wanted:
            raise SystemExit(f"no transport-input asset matches {args.only!r}")

    out.mkdir(parents=True, exist_ok=True)
    total = 0
    unsupported = []
    for fn in sorted(wanted):
        spec = wanted[fn]
        path = out / fn
        if spec["format"] not in WRITERS:
            unsupported.append((fn, spec["format"]))
            continue
        frames = DEFAULT_FRAMES
        est = int(spec["width"] * spec["height"] * WRITERS[spec["format"]][1] * frames)
        print(
            f"  video {fn}  {spec['width']}x{spec['height']} "
            f"{spec['format']} x{frames} ~{est / 1e6:.0f} MB"
        )
        total += est
        if args.dry_run:
            continue
        if path.exists() and not args.force:
            print("    exists, skipped (use --force to replace)")
            continue
        if not _write_video(path, spec, frames, args.force):
            print("    created concurrently, skipped (use --force to replace)")

    print(f"\n{len(wanted)} assets, ~{total / 1e9:.2f} GB into {out}")
    if unsupported:
        print("\nNo writer for:")
        for fn, fmt in unsupported:
            print(f"  {fn}  ({fmt})")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
