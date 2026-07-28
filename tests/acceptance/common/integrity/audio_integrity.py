# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
# Media Communications Mesh

import argparse
import logging
import sys
from pathlib import Path

from video_integrity import calculate_chunk_hashes


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

        src_frames = len(src_chunk_sums)
        out_frames = len(out_chunk_sums)

        # In some pipelines the input is looped and transmitted multiple times.
        # In that case the output may contain multiple repetitions of the source.
        # We only validate the first `src_frames` frames and ignore any trailing frames.
        if out_frames > src_frames:
            self.logger.info(
                f"Output contains {out_frames} frames; source contains {src_frames} frames. "
                f"Ignoring {out_frames - src_frames} trailing frames (looped output)."
            )

        frames_to_check = min(src_frames, out_frames)
        bad_frames = 0
        for idx in range(frames_to_check):
            if out_chunk_sums[idx] != src_chunk_sums[idx]:
                self.logger.error(f"Bad audio frame at index {idx} in {out_url}")
                bad_frames += 1
        if bad_frames:
            self.logger.error(
                f"Received {bad_frames} bad frames out of {frames_to_check} checked."
            )
            return False

        self.logger.info(
            f"All {frames_to_check} checked frames in {out_url} are correct."
        )
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
        self.logger.info("All frames in stream segments are correct.")
        return True


def main():
    # Set up logging
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s",
    )
    logger = logging.getLogger(__name__)

    # Create the argument parser
    parser = argparse.ArgumentParser(
        description="Audio Integrity Checker",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(
        dest="mode", help="Operation mode", required=True
    )

    # Common arguments for both file and stream modes
    def add_common_arguments(parser):
        parser.add_argument("src", help="Source audio file path")
        parser.add_argument("out", help="Output audio file name (without extension)")
        parser.add_argument(
            "--sample_size",
            type=int,
            default=2,
            help="Audio sample size in bytes (default: 2)",
        )
        parser.add_argument(
            "--sample_num",
            type=int,
            default=480,
            help="Number of samples per frame (default: 480)",
        )
        parser.add_argument(
            "--channel_num",
            type=int,
            default=2,
            help="Number of audio channels (default: 2)",
        )
        parser.add_argument(
            "--output_path",
            type=str,
            default="/mnt/ramdisk",
            help="Output path (default: /mnt/ramdisk)",
        )
        parser.add_argument(
            "--delete_file",
            action="store_true",
            default=True,
            help="Delete output files after processing (default: True)",
        )
        parser.add_argument(
            "--no_delete_file",
            action="store_false",
            dest="delete_file",
            help="Do NOT delete output files after processing",
        )

    # Stream mode parser
    stream_help = """Check integrity for audio stream (stream saved into files segmented by time)

It assumes that there is X digit segment number in the file name like `out_name_001.pcm` or `out_name_02.pcm`.
It can be achieved by using ffmpeg with `-f segment` option.

Example: ffmpeg -i input.wav -f segment -segment_time 3 out_name_%03d.pcm"""
    stream_parser = subparsers.add_parser(
        "stream",
        help="Check integrity for audio stream (segmented files)",
        description=stream_help,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_common_arguments(stream_parser)
    stream_parser.add_argument(
        "--segment_duration",
        type=int,
        default=3,
        help="Segment duration in seconds (default: 3)",
    )

    # File mode parser
    file_help = """Check integrity for single audio file.

This mode compares a single output audio file against a source reference file.
It performs frame-by-frame integrity checking using MD5 checksums."""
    file_parser = subparsers.add_parser(
        "file",
        help="Check integrity for single audio file",
        description=file_help,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_common_arguments(file_parser)

    # Parse the arguments
    args = parser.parse_args()

    # Execute based on mode
    if args.mode == "stream":
        integrator = AudioStreamIntegritor(
            logger,
            args.src,
            args.out,
            args.sample_size,
            args.sample_num,
            args.channel_num,
            args.output_path,
            args.delete_file,
        )
        result = integrator.check_stream_integrity()
    elif args.mode == "file":
        # For file mode, construct the full output file path
        out_file = Path(args.output_path) / args.out
        integrator = AudioFileIntegritor(
            logger,
            args.src,
            args.out,
            args.sample_size,
            args.sample_num,
            args.channel_num,
            args.output_path,
            args.delete_file,
        )
        result = integrator.check_integrity_file(str(out_file))
    else:
        parser.print_help()
        return

    if result:
        logging.info("Audio integrity check passed")
    else:
        logging.error("Audio integrity check failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
