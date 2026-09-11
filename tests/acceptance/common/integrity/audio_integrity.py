# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
# Media Communications Mesh

import argparse
import logging
import sys
from pathlib import Path

# The RX joins a running stream, so MTL drops its first short frames: at worst
# two whole frames inside the first five on this suite.
JOIN_FRAMES = 5
MAX_JOIN_GAP_FRAMES = 2

# Per-frame error lines a failing check may emit; an unbounded report fills the
# SSH channel window and hangs the run (see integrity_runner.py).
MAX_BAD_FRAME_REPORTS = 20

# Places the capture first frame may match in the source loop. Silence or a flat
# pattern matches nearly everywhere: that is a vacuous pass, not a check.
MAX_START_POSITIONS = 64


def get_pcm_frame_size(sample_size: int, sample_num: int, channel_num: int) -> int:
    return sample_size * sample_num * channel_num


class AudioIntegritor:
    """Compares a capture against the loop of the source file the TX sends.

    A correct capture is a window onto that loop starting wherever the RX joined,
    not a copy from byte 0. RxTxApp loops whole frames, ffmpeg the whole file, so
    both periods are tried.
    """

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
        min_frames: int = 0,
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
        self.min_frames = min_frames
        with open(src_url, "rb") as src_file:
            self.src = src_file.read()
        if len(self.src) < self.frame_size:
            raise ValueError(
                f"{src_url} holds {len(self.src)} bytes, less than one "
                f"{self.frame_size}-byte frame"
            )
        group = sample_size * channel_num
        whole_frames_end = len(self.src) // self.frame_size * self.frame_size
        whole_groups_end = len(self.src) // group * group
        self.periods = sorted({whole_frames_end, whole_groups_end})
        self._repeats = {}

    def _repeated(self, period: int) -> bytes:
        """The first *period* bytes plus one frame of wrap, so a loop-straddling
        window is a slice. One frame is all either consumer reads past a start
        position, and a start position is always below *period*."""
        if period not in self._repeats:
            self._repeats[period] = self.src[:period] + self.src[: self.frame_size]
        return self._repeats[period]

    def _start_positions(self, out: bytes, period: int):
        """Byte positions in the source loop where *out* can begin, or None if it
        could begin in too many to tell. Each candidate is then compared in full."""
        repeated = self._repeated(period)
        probe = out[: self.frame_size]
        positions, at = [], 0
        while True:
            at = repeated.find(probe, at)
            if at < 0 or at >= period:
                return positions
            positions.append(at)
            if len(positions) > MAX_START_POSITIONS:
                return None
            at += 1

    def _compare(
        self,
        out: bytes,
        period: int,
        position: int,
        report: bool = False,
        joining_window: int = JOIN_FRAMES,
    ) -> tuple:
        """Compare *out* against the loop of *period* bytes starting at *position*.

        Returns (first differing frame index or None, differing frames, frames
        missing at the join). With *report* off it returns at the first difference,
        so a wrong start position costs one comparison. Within the first
        *joining_window* frames a frame may instead match up to MAX_JOIN_GAP_FRAMES
        frames on -- audio sent before the RX joined; the source position only moves
        forward, so no written or unsent audio is absorbed.
        """
        size, repeated = self.frame_size, self._repeated(period)
        first_bad, bad_frames = None, 0
        missing, src_at = 0, position
        for index in range(-(-len(out) // size)):
            chunk = out[index * size : (index + 1) * size]
            joining = index < joining_window
            for gap in range(MAX_JOIN_GAP_FRAMES - missing + 1 if joining else 1):
                at = (src_at + gap * size) % period
                if chunk == repeated[at : at + len(chunk)]:
                    missing += gap
                    src_at = (at + size) % period
                    break
            else:
                bad_frames += 1
                if first_bad is None:
                    first_bad = index
                if not report:
                    return first_bad, bad_frames, missing
                if bad_frames <= MAX_BAD_FRAME_REPORTS:
                    self.logger.error(
                        f"Bad audio frame at output index {index} "
                        f"(source byte {src_at})"
                    )
                elif bad_frames == MAX_BAD_FRAME_REPORTS + 1:
                    self.logger.error(
                        f"Suppressing further per-frame errors after "
                        f"{MAX_BAD_FRAME_REPORTS}; see the totals below."
                    )
                src_at = (src_at + size) % period
        return first_bad, bad_frames, missing

    def check_integrity_file(
        self, out_url, joining_window: int = JOIN_FRAMES, out: bytes | None = None
    ) -> bool:
        if out is None:  # given *out*, *out_url* only names the capture in reports
            with open(out_url, "rb") as out_file:
                out = out_file.read()
        size = self.frame_size
        out_frames = len(out) // size
        # A short final frame is compared too, so it counts as one.
        out_chunks = -(-len(out) // size)
        self.logger.info(
            f"Checking integrity for src {self.src_url} ({len(self.src)} bytes) "
            f"and out {out_url} ({len(out)} bytes, {out_frames} frames) "
            f"with frame size {size}"
        )

        if not out_frames:
            self.logger.error(f"{out_url} holds no full {size}-byte frame to check")
            return False

        if out_frames < self.min_frames:
            self.logger.error(
                f"{out_url} holds {out_frames} frames, fewer than the "
                f"{self.min_frames} a complete run captures."
            )
            return False

        best = None  # (first bad frame, period, position)
        for period in self.periods:
            positions = self._start_positions(out, period)
            if positions is None:
                self.logger.error(
                    f"{out_url} starts in over {MAX_START_POSITIONS} places at period "
                    f"{period} in {self.src_url}: join point cannot be told (silence?)"
                )
                return False
            for position in positions:
                first_bad, _, missing = self._compare(
                    out, period, position, joining_window=joining_window
                )
                if first_bad is None:
                    if missing:
                        self.logger.warning(
                            f"{out_url} is missing {missing} frame(s) of audio "
                            f"within its first {joining_window} while joining."
                        )
                    self.logger.info(
                        f"All {out_chunks} frames of {out_url} are correct "
                        f"(source byte {position}, loop period {period})."
                    )
                    return True
                if best is None or first_bad > best[0]:
                    best = (first_bad, period, position)

        if best is None:
            self.logger.error(
                f"No position in {self.src_url} explains the leading frames of "
                f"{out_url}, at either loop period {self.periods}."
            )
            return False

        first_bad, period, position = best
        _, bad_frames, _ = self._compare(
            out, period, position, report=True, joining_window=joining_window
        )
        self.logger.error(
            f"{out_url} follows {self.src_url} from source byte {position} "
            f"(loop period {period}) but {bad_frames} of its {out_chunks} "
            f"frames differ, the first at index {first_bad}."
        )
        return False


class AudioStreamIntegritor(AudioIntegritor):
    def get_out_files(self):
        """Segments in capture order. Plain sorting misorders them as soon as the
        counter grows a digit, so shorter names -- lower numbers -- come first."""
        files = Path(self.out_path).glob(f"{self.out_name}*")
        return sorted(files, key=lambda path: (len(path.name), path.name))

    def check_stream_integrity(self) -> bool:
        out_files = self.get_out_files()
        if not out_files:
            self.logger.error(
                f"No output files found for stream in {self.out_path} with prefix {self.out_name}"
            )
            return False
        # The segments are consecutive cuts of one capture, so check them joined.
        # Checked one by one, a segment re-anchors on the source wherever it can,
        # which hides audio lost across a boundary; joined, only the head of the
        # stream carries the join allowance, which is where the RX joined.
        joined = b"".join(out_file.read_bytes() for out_file in out_files)
        ok = self.check_integrity_file(
            f"{len(out_files)} stream segments "
            f"{out_files[0].name}..{out_files[-1].name}",
            out=joined,
        )
        if self.delete_file:
            for out_file in out_files:
                out_file.unlink()
        return ok


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
The transmitter loops the source, so the capture is compared frame by frame
against that loop starting wherever the receiver joined it."""
    file_parser = subparsers.add_parser(
        "file",
        help="Check integrity for single audio file",
        description=file_help,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_common_arguments(file_parser)
    file_parser.add_argument(
        "--min_frames",
        type=int,
        default=0,
        help="Fail if the capture holds fewer frames than this (default: 0)",
    )

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
        integrator = AudioIntegritor(
            logger,
            args.src,
            args.out,
            args.sample_size,
            args.sample_num,
            args.channel_num,
            args.output_path,
            args.delete_file,
            args.min_frames,
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
