# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
# Media Communications Mesh

import hashlib
import logging
from pathlib import Path


def calculate_chunk_hashes(file_url: str, chunk_size: int) -> list:
    chunk_sums = []
    with open(file_url, "rb") as f:
        chunk_index = 0
        while chunk := f.read(chunk_size):
            if len(chunk) != chunk_size:
                logging.debug(
                    f"CHUNK SIZE MISMATCH at index {chunk_index}: {len(chunk)} != {chunk_size}"
                )
            chunk_sum = hashlib.md5(chunk).hexdigest()
            chunk_sums.append(chunk_sum)
            chunk_index += 1
    return chunk_sums


def get_pcm_frame_size(sample_size: int, sample_num: int, channel_num: int) -> int:
    return sample_size * sample_num * channel_num


class AudioIntegritor:
    def __init__(
        self,
        logger: logging.Logger,
        src_url: str,
        out_name: str,
        sample_size: int = 2,
        sample_num: int = 480,
        channel_num: int = 2,
        out_path: str = "/mnt/ramdisk",
        delete_file: bool = True,
    ):
        self.logger = logger
        self.src_url = src_url
        self.out_name = out_name
        self.sample_size = sample_size
        self.sample_num = sample_num
        self.channel_num = channel_num
        self.frame_size = get_pcm_frame_size(sample_size, sample_num, channel_num)
        self.out_path = out_path
        self.delete_file = delete_file
        self.src_chunk_sums = calculate_chunk_hashes(src_url, self.frame_size)


class AudioFileIntegritor(AudioIntegritor):
    def check_integrity_file(self, out_url) -> bool:
        self.logger.info(
            f"Checking integrity for src {self.src_url} and out {out_url} "
            f"with frame size {self.frame_size}"
        )
        src_chunk_sums = self.src_chunk_sums
        out_chunk_sums = calculate_chunk_hashes(out_url, self.frame_size)
        bad_frames = 0
        for idx, chunk_sum in enumerate(out_chunk_sums):
            if idx >= len(src_chunk_sums) or chunk_sum != src_chunk_sums[idx]:
                self.logger.error(f"Bad audio frame at index {idx} in {out_url}")
                bad_frames += 1
        if bad_frames:
            self.logger.error(
                f"Received {bad_frames} bad frames out of {len(out_chunk_sums)} checked."
            )
            return False
        self.logger.info(f"All {len(out_chunk_sums)} frames in {out_url} are correct.")
        return True


class AudioStreamIntegritor(AudioIntegritor):
    def get_out_files(self):
        return sorted(Path(self.out_path).glob(f"{self.out_name}*"))

    def check_stream_integrity(self) -> bool:
        bad_frames_total = 0
        out_files = self.get_out_files()
        if not out_files:
            self.logger.error(
                f"No output files found for stream in {self.out_path} with prefix {self.out_name}"
            )
            return False
        for out_file in out_files:
            self.logger.info(f"Checking integrity for segment file: {out_file}")
            out_chunk_sums = calculate_chunk_hashes(str(out_file), self.frame_size)
            for idx, chunk_sum in enumerate(out_chunk_sums):
                if (
                    idx >= len(self.src_chunk_sums)
                    or chunk_sum != self.src_chunk_sums[idx]
                ):
                    self.logger.error(f"Bad audio frame at index {idx} in {out_file}")
                    bad_frames_total += 1
            if self.delete_file:
                out_file.unlink()
        if bad_frames_total:
            self.logger.error(
                f"Received {bad_frames_total} bad frames in stream segments."
            )
            return False
        self.logger.info(f"All frames in stream segments are correct.")
        return True
