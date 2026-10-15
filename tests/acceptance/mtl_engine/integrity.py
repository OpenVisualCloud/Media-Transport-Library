# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import hashlib
import re
from math import floor

from .execute import log_fail

# Fraction of the nominal frame count a recording must reach to count as complete.
MIN_RECORDED_FRAME_RATIO = 0.5


def min_expected_frames(fps: float, test_time: float) -> int:
    """Frames a *test_time*-second recording at *fps* must hold to be complete.

    RX opens before TX and both are killed on a timer, and pacing training eats
    the first seconds, so healthy recordings land well short of nominal. Never 0
    -- an empty recording must never pass.
    """
    return max(1, int(fps * test_time * MIN_RECORDED_FRAME_RATIO))


def check_st20p_integrity(
    src_url: str, out_url: str, frame_size: int, skip_frames: int = 1
):
    src_chunk_sums = []

    with open(src_url, "rb") as f:
        while chunk := f.read(frame_size):
            chunk_sum = hashlib.md5(chunk, usedforsecurity=False).hexdigest()
            src_chunk_sums.append(chunk_sum)

    out_chunk_sums = []

    with open(out_url, "rb") as f:
        while chunk := f.read(frame_size):
            chunk_sum = hashlib.md5(chunk, usedforsecurity=False).hexdigest()
            out_chunk_sums.append(chunk_sum)

    # Skip initial frames that may be corrupted during session establishment
    src_chunk_sums = src_chunk_sums[skip_frames:]
    out_chunk_sums = out_chunk_sums[skip_frames:]

    if len(src_chunk_sums) < len(out_chunk_sums):
        for i in range(len(src_chunk_sums)):
            if src_chunk_sums[i] != out_chunk_sums[i]:
                log_fail(f"Received frame {i + skip_frames} is invalid")
                return False
    else:
        for i in range(len(out_chunk_sums)):
            if out_chunk_sums[i] != src_chunk_sums[i]:
                log_fail(f"Received frame {i + skip_frames} is invalid")
                return False

    return True


def calculate_yuv_frame_size(width: int, height: int, file_format: str):
    match file_format:
        case "YUV422RFC4175PG2BE10":
            pixel_size = 2.5
        case "YUV422PLANAR10LE":
            pixel_size = 4
        case _:
            log_fail(f"Size of {file_format} pixel is not known")

    return int(width * height * pixel_size)


def check_st30p_integrity(src_url: str, out_url: str, size: int):
    src_chunks = []

    with open(src_url, "rb") as f:
        while chunk := f.read(size):
            src_chunks.append(chunk)

    out_chunks = []

    with open(out_url, "rb") as f:
        while chunk := f.read(size):
            out_chunks.append(chunk)

    if len(src_chunks) < len(out_chunks):
        for i in range(len(src_chunks)):
            if src_chunks[i] != out_chunks[i]:
                log_fail(f"Received frame {i} is invalid")
                return False
    else:
        for i in range(len(out_chunks)):
            if out_chunks[i] != src_chunks[i]:
                log_fail(f"Received frame {i} is invalid")
                return False

    return True


def get_sample_size(format: str) -> int:
    match format:
        case "PCM8":
            return 1
        case "PCM16":
            return 2
        case "PCM24":
            return 3
        case _:
            log_fail(f"Unknown audio format: {format}")
            return 0


def get_sample_number(sampling: str, ptime: str) -> int:
    """Samples per channel in one RTP packet of *ptime* at *sampling*."""
    match sampling:
        case "48kHz":
            match ptime:
                case "1":
                    return 48
                case "0.12":
                    return 6
                case "0.25":
                    return 12
                case "0.33":
                    return 16
                case "4":
                    return 192
        case "96kHz":
            match ptime:
                case "1":
                    return 96
                case "0.12":
                    return 12
                case "0.25":
                    return 24
                case "0.33":
                    return 32
                case "4":
                    return 384
    log_fail(f"Unknown audio sampling/ptime combination: {sampling}/{ptime}")
    return 0


# Samples per second per channel, keyed by the sampling string tests pass.
_SAMPLING_HZ = {"48kHz": 48000, "96kHz": 96000}

# An st30p frame holds as many whole ptime packets as fit in 10 ms
# (st30_calculate_framebuff_size, lib/src/st2110/st_fmt.c).
_FRAME_TIME_MS = 10


def get_frame_sample_number(sampling: str, ptime: str) -> int:
    """Samples per channel in one st30p *frame*, not one packet -- the unit the
    checker compares and RxTxApp's TX wraps its source file by."""
    packet_samples = get_sample_number(sampling, ptime)
    if not packet_samples:
        # get_sample_number already reported the unknown combination.
        return 0

    frame_samples = _SAMPLING_HZ[sampling] * _FRAME_TIME_MS // 1000
    return frame_samples // packet_samples * packet_samples


def get_min_frame_number(sampling: str, ptime: str, test_time: int) -> int:
    """Frames a healthy RX must capture in *test_time* seconds -- a truncated
    capture is bit-exact, so only length catches it."""
    frame_samples = get_frame_sample_number(sampling, ptime)
    if not frame_samples:
        return 0
    return min_expected_frames(_SAMPLING_HZ[sampling] / frame_samples, test_time)


def get_channel_number(channel: str) -> int:
    match channel:
        case "M":
            return 1
        case "DM" | "ST" | "LtRt" | "AES3":
            return 2
        case "51":
            return 6
        case "71":
            return 8
        case "222":
            return 24
        case "SGRP":
            return 4
        case _:
            match = re.match(r"^U(\d{2})$", channel)

            if match:
                return int(match.group(1))
    log_fail(f"Unknown audio channel: {channel}")
    return 0


def calculate_st30p_framebuff_size(
    format: str, ptime: str, sampling: str, channel: str
):
    sample_size = get_sample_size(format)
    sample_num = get_sample_number(sampling, ptime)
    channel_num = get_channel_number(channel)

    packet_size = sample_size * sample_num * channel_num

    match ptime:
        case "1":
            packet_time = 1_000_000 * 1
        case "0.12":
            packet_time = 1_000_000 * 0.125
        case "0.25":
            packet_time = 1_000_000 * 0.25
        case "0.33":
            packet_time = 1_000_000 * 1 / 3
        case "4":
            packet_time = 1_000_000 * 4

    desired_frame_time = 10_000_000

    packet_per_frame = 1

    if desired_frame_time > packet_time:
        packet_per_frame = floor(desired_frame_time / packet_time)

    return packet_per_frame * packet_size


# Each st40p TX (RxTxApp tx_st40p_app.c, mtl_st40p_tx, FFmpeg mtl_st40p) starts
# every pass over its input at byte 0 and cuts it into frames of at most 255 UDW
# bytes (RFC 8331) or into 1024-byte data items (fast metadata,
# ST_APP_FMD_CHUNK_BYTES), the last one of a pass shorter.
ANC_FRAME_BYTES = 255
FMD_ITEM_BYTES = 1024
_FRAME_INFO_FIELD = re.compile(r"(\w+)=(\d+)")


def st40p_pass(src_size: int, fast_metadata: bool) -> tuple[int, int]:
    """``(bytes, frames)`` of one TX pass over the input, as RX records it.

    RX gets each data item zero padded to a 32-bit word, and only the last
    item of a pass is not a whole number of words.
    """
    if fast_metadata:
        return src_size + -src_size % 4, -(-src_size // FMD_ITEM_BYTES)
    return src_size, -(-src_size // ANC_FRAME_BYTES)


def st40p_frame_info_path(out: str) -> str:
    """The per-frame log the st40p RX writes beside its recording."""
    return f"{out}.info"


def _st40p_repeats_cmd(src, out, src_size, out_size, fast_metadata) -> str:
    """A host command that passes iff *out* holds the passes over *src* from
    byte 0, the last one possibly cut."""
    unit, _ = st40p_pass(src_size, fast_metadata)
    if not src_size or out_size < unit:
        return "false"
    cmds = [f"cmp -s -n {src_size} {src} {out}"]
    if unit > src_size:
        # The zero padding of the last data item of a pass.
        cmds.append(f"cmp -s -n {unit - src_size} -i {src_size}:0 {out} /dev/zero")
    if out_size > unit:
        # Each pass equals the one before it.
        cmds.append(f"cmp -s -n {out_size - unit} {out} {out} {unit} 0")
    return " && ".join(cmds)


def st40p_recording_errors(host, params: dict, out: str, min_frames: int) -> list:
    """Why an st40p RX recording is not the TX input, empty if it is.

    The recording must hold the TX passes over ``input_file`` byte for byte,
    and every line of its frame-info log ``seq_discont=0 seq_lost=0``. A fast
    metadata data item must also carry marker 0 (ST 2110-41 §5.2) and the DIT
    and K-bit TX set. The host runs ``cmp`` and ``uniq -c``, so only a few
    lines cross the SSH channel.
    """
    src, fmd = params["input_file"], params["fast_metadata"]
    dit = int(params["fastmetadata_data_item_type"])
    k_bit = int(params["fastmetadata_k_bit"])

    def run(cmd):
        return host.connection.execute_command(
            cmd, shell=True, expected_return_codes=None
        )

    errors = []
    res = run(f"stat -c %s {src} {out}")
    src_size, out_size = map(int, res.stdout.split()) if not res.return_code else (0, 0)
    if run(_st40p_repeats_cmd(src, out, src_size, out_size, fmd)).return_code:
        errors.append(
            f"{out} ({out_size} B) is not {src} ({src_size} B) repeated"
            + (" in passes padded to 32-bit words" if fmd else "")
        )

    log = st40p_frame_info_path(out)
    summary = run(f"sed 's/^ts=[0-9]* //' {log} | sort | uniq -c").stdout
    frames = lost = bad = 0
    for line in summary.splitlines():
        count, _, rest = line.strip().partition(" ")
        if not count.isdigit():
            continue
        n, f = int(count), dict(_FRAME_INFO_FIELD.findall(rest))
        frames += n
        if f.get("seq_discont") != "0" or f.get("seq_lost") != "0":
            lost += n
        header = (f.get("rtp_marker"), f.get("fmd_dit"), f.get("fmd_k_bit"))
        if fmd and header != ("0", str(dit), str(k_bit)):
            bad += n
    if frames < min_frames:
        errors.append(f"{log} holds {frames} frames, expected >= {min_frames}")
    if lost:
        errors.append(f"{lost} frames show a lost or reordered packet")
    if bad:
        errors.append(f"{bad} data items lack marker 0, DIT {dit:#x} or K-bit {k_bit}")
    return errors
