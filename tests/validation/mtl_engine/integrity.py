# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import hashlib
import re
from math import floor

from .execute import log_fail


def check_st20p_integrity(src_url: str, out_url: str, frame_size: int):
    src_chunk_sums = []

    with open(src_url, "rb") as f:
        while chunk := f.read(frame_size):
            chunk_sum = hashlib.md5(chunk).hexdigest()
            src_chunk_sums.append(chunk_sum)

    out_chunk_sums = []

    with open(out_url, "rb") as f:
        while chunk := f.read(frame_size):
            chunk_sum = hashlib.md5(chunk).hexdigest()
            out_chunk_sums.append(chunk_sum)

    if len(src_chunk_sums) < len(out_chunk_sums):
        for i in range(len(src_chunk_sums)):
            if src_chunk_sums[i] != out_chunk_sums[i]:
                log_fail(f"Received frame {i} is invalid")
                return False
    else:
        for i in range(len(out_chunk_sums)):
            if out_chunk_sums[i] != src_chunk_sums[i]:
                log_fail(f"Received frame {i} is invalid")
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
    return 0


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
