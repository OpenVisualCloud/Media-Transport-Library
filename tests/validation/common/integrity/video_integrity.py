# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
# Media Communications Mesh

import argparse
import hashlib
import logging
import multiprocessing
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import cv2
    import pytesseract
except ImportError:
    pass


STARTING_FRAME = 1  # The frame number from which processing starts.
TEXT_POSITION_X = 0  # X-coordinate for text overlay on video frames.
TEXT_POSITION_Y = 20  # Y-coordinate for text overlay on video frames.
TEXT_FONT_SCALE = 10  # Font scale for text overlay on video frames.
TEXT_LENGTH = 350  # Maximum length of text overlay on video frames.
FIRST_FRAME_NAME = "output1.png"


def calculate_chunk_hashes(file_url: str, chunk_size: int) -> list:
    chunk_sums = []
    with open(file_url, "rb") as f:
        chunk_index = 0  # Initialize a counter for the chunk index
        while chunk := f.read(chunk_size):
            if len(chunk) != chunk_size:
                logging.debug(
                    f"CHUNK SIZE MISMATCH at index {chunk_index}: {len(chunk)} != {chunk_size}"
                )
            chunk_sum = hashlib.md5(chunk).hexdigest()
            chunk_sums.append(chunk_sum)
            chunk_index += 1  # Increment the chunk index
    return chunk_sums


def calculate_yuv_frame_size(width: int, height: int, file_format: str) -> int:
    match file_format:
        case "YUV422RFC4175PG2BE10" | "yuv422p10rfc4175":
            pixel_size = 2.5
        case "YUV422PLANAR10LE" | "yuv422p10le":
            pixel_size = 4
        case _:
            logging.error(f"Size of {file_format} pixel is not known")
            raise ValueError(f"Size of {file_format} pixel is not known")
    return int(width * height * pixel_size)


class VideoIntegritor:
    def __init__(
        self,
        logger: logging.Logger,
        src_url: str,
        out_name: str,
        resolution: str,
        file_format: str = "yuv422p10le",
        out_path: str = "/mnt/ramdisk",
        delete_file: bool = True,
    ):
        self.logger = logger
        self.logger.info(
            f"Verify integrity for src {src_url} out file names: {out_name}\nresolution: {resolution}, "
            + f"file_format: {file_format}"
        )
        self.resolution = resolution
        self.file_format = file_format
        self.width, self.height = map(int, resolution.split("x"))
        self.frame_size = calculate_yuv_frame_size(self.width, self.height, file_format)
        self.src_chunk_sums = calculate_chunk_hashes(src_url, self.frame_size)
        self.src_url = src_url
        self.out_name = out_name
        self.out_path = out_path
        self.shift = None
        self.out_frame_no = 0
        self.bad_frames_total = 0
        self.delete_file = delete_file

    def shift_src_chunk_by_first_frame_no(self, out_file):
        if "cv2" not in sys.modules or "pytesseract" not in sys.modules:
            self.logger.warning(
                "cv2/pytesseract not available; falling back to hash-based alignment."
            )
            try:
                with open(out_file, "rb") as handle:
                    first_chunk = handle.read(self.frame_size)
                if len(first_chunk) != self.frame_size:
                    raise ValueError(
                        f"Output file {out_file} is smaller than one frame"
                    )
                first_sum = hashlib.md5(first_chunk).hexdigest()
                if first_sum in self.src_chunk_sums:
                    shift_index = self.src_chunk_sums.index(first_sum)
                    self.shift = shift_index + STARTING_FRAME
                    for _ in range(shift_index):
                        self.src_chunk_sums.append(self.src_chunk_sums.pop(0))
                    self.logger.info(
                        "Aligned source chunks by hash using first frame index %s",
                        shift_index,
                    )
                else:
                    self.logger.warning(
                        "First frame hash not found in source; proceeding without shift."
                    )
            except Exception as exc:
                self.logger.warning(
                    "Hash-based alignment failed (%s); proceeding without shift.",
                    exc,
                )
            return
        font_size = self.height // TEXT_FONT_SCALE
        custom_config = r"--oem 3 --psm 6 -c tessedit_char_whitelist=0123456789"
        cmd = [
            "ffmpeg",
            "-s",
            self.resolution,
            "-pix_fmt",
            self.file_format,
            "-i",
            str(out_file),
            "-vframes",
            "1",
            "-update",
            "1",
            FIRST_FRAME_NAME,
            "-y",
        ]
        self.logger.info(f"Running command: {' '.join(cmd)}")
        rn = subprocess.run(cmd, capture_output=True, text=True)
        self.logger.debug(f"Command output: {rn.returncode}, {rn.stdout}, {rn.stderr}")
        img_out = cv2.imread(FIRST_FRAME_NAME)
        height = TEXT_POSITION_Y + font_size
        width = TEXT_POSITION_X + TEXT_LENGTH
        # Define the region of interest (ROI) for text extraction
        roi = img_out[0:height, 0:width]
        # cv2.imwrite("output_roi.png", roi)
        val = pytesseract.image_to_string(roi, config=custom_config)
        match = re.match(r"\d{5}", val)
        if match:
            self.shift = int(match.group(0))
            self.logger.info(f"Extracted first captured frame number: {self.shift}")
            for _ in range(self.shift - STARTING_FRAME):
                self.src_chunk_sums.append(self.src_chunk_sums.pop(0))
        else:
            raise ValueError(
                f"No match found in the extracted text from first frame of {self.src_url}"
            )

    def check_integrity_file(self, out_url) -> bool:
        out_chunk_sums = calculate_chunk_hashes(out_url, self.frame_size)
        bad_frames = 0
        for ids, chunk_sum in enumerate(out_chunk_sums):
            if chunk_sum != self.src_chunk_sums[ids % len(self.src_chunk_sums)]:
                self.logger.error(f"Received bad frame number {ids} in {out_url}.")
                if chunk_sum in self.src_chunk_sums:
                    self.logger.error(
                        f"Chunk: {chunk_sum} received in position: {ids}, "
                        + f"expected in {self.src_chunk_sums.index(chunk_sum)}"
                    )
                else:
                    self.logger.error(
                        f"Chunk: {chunk_sum} with ID: {ids} not found in src chunks checksums!"
                    )
                bad_frames += 1
        if bad_frames:
            self.bad_frames_total += bad_frames
            self.logger.error(
                f"Received {bad_frames} bad frames out of {len(out_chunk_sums)} captured."
            )
            return False
        self.logger.info(f"All {len(out_chunk_sums)} frames in {out_url} are correct.")
        return True

    def _worker(self, wkr_id, request_queue, result_queue, shared):
        while True:
            out_url = request_queue.get()
            self.logger.info(f"worker {wkr_id} Checking  file: {out_url}")
            if out_url:
                if out_url == "Done":
                    break
                result = self.check_integrity_file(out_url)
                if not result and shared.get("error_file", None) is None:
                    shared["error_file"] = out_url
                    self.logger.debug(f"Saved error file {out_url} for later analysis.")
                    continue
                if self.delete_file:
                    out_url.unlink()
        result_queue.put(self.bad_frames_total)


class VideoStreamIntegritor(VideoIntegritor):
    """Class to check integrity of video stream saved into files divided by segments.
    It assumes that there is X digit segment number in the file name like `out_name_001.yuv` or `out_name_02.yuv`.
    """

    def __init__(
        self,
        logger: logging.Logger,
        src_url: str,
        out_name: str,
        resolution: str,
        file_format: str = "yuv422p10le",
        out_path: str = "/mnt/ramdisk",
        segment_duration: int = 3,
        workers_count=5,
        delete_file: bool = True,
    ):
        super().__init__(
            logger, src_url, out_name, resolution, file_format, out_path, delete_file
        )
        self.logger.info(
            f"Output path {out_path}, segment duration: {segment_duration}"
        )
        self.segment_duration = segment_duration
        self.workers_count = workers_count

    def get_out_file(self, request_queue):
        self.logger.info(
            f"Waiting for output files from {self.out_path} with prefix {self.out_name}"
        )
        start = 0
        waiting_for_files = True
        list_processed = []
        out_files = []

        while (self.segment_duration + 1 > start) or waiting_for_files:
            gb = list(Path(self.out_path).glob(f"{self.out_name}*"))
            out_files = list(filter(lambda x: x not in list_processed, gb))
            self.logger.debug(f"Received files: {out_files}")
            if len(out_files) > 1:
                self.logger.debug(f"Puting file into queue {out_files[0]}")
                request_queue.put(out_files[0])
                list_processed.append(out_files[0])
                waiting_for_files = False
                start = 0
            start += 0.5
            time.sleep(0.5)
        else:
            try:
                request_queue.put(out_files[0])
            except IndexError:
                self.logger.info(f"No more output files found in {self.out_path}")

    def worker(self, wkr_id, request_queue, result_queue, shared):
        if wkr_id == 0:
            self.logger.info(f"Trying to find shift in worker {wkr_id}")
            while True:
                first_file = request_queue.get()
                if first_file:
                    break
            self.logger.info(f"worker {wkr_id} Checking first file: {first_file}")
            self.shift_src_chunk_by_first_frame_no(first_file)
            shared["src_chksums"] = self.src_chunk_sums
            shared["shift"] = self.shift
            result = self.check_integrity_file(first_file)
            if not result:
                shared["error_file"] = first_file
            elif self.delete_file:
                self.logger.info(f"Removing first file {first_file}")
                first_file.unlink()
        else:
            while shared.get("shift", None) is None:
                time.sleep(0.1)  # wait until first worker find shift
            else:
                self.src_chunk_sums = shared["src_chksums"]
        self._worker(wkr_id, request_queue, result_queue, shared)

    def start_workers(self, result_queue, request_queue, shared):
        # Create and start a separate process for each task
        processes = []
        for number in range(self.workers_count):
            p = multiprocessing.Process(
                target=self.worker, args=(number, request_queue, result_queue, shared)
            )
            p.start()
            processes.append(p)
        return processes

    def check_st20p_integrity(self) -> bool:
        mgr = multiprocessing.Manager()
        shared_data = mgr.dict()
        result_queue = multiprocessing.Queue()
        request_queue = multiprocessing.Queue()
        workers = self.start_workers(result_queue, request_queue, shared_data)
        output_worker = multiprocessing.Process(
            target=self.get_out_file, args=(request_queue,)
        )
        output_worker.start()
        results = []
        while output_worker.is_alive():
            time.sleep(1)
        output_worker.join()
        for wk in workers:
            request_queue.put("Done")
        for wk in workers:
            results.append(result_queue.get())
            wk.join()
        logging.info(f"Results of bad frames: {results}")
        return True if shared_data.get("error_file", None) is None else False


class VideoFileIntegritor(VideoIntegritor):

    def __init__(
        self,
        logger: logging.Logger,
        src_url: str,
        out_name: str,
        resolution: str,
        file_format: str = "yuv422p10le",
        out_path: str = "/mnt/ramdisk",
        delete_file: bool = True,
    ):
        super().__init__(
            logger, src_url, out_name, resolution, file_format, out_path, delete_file
        )
        self.logger = logging.getLogger(__name__)

    def get_out_file(self):
        try:
            gb = list(Path(self.out_path).glob(f"{self.out_name}*"))
            return gb[0]
        except IndexError:
            self.logger.error(f"File {self.out_name} not found!")
            raise FileNotFoundError(
                f"File {self.out_name} not found in {self.out_path}"
            )

    def check_st20p_integrity(self) -> bool:
        output_file = self.get_out_file()
        self.shift_src_chunk_by_first_frame_no(output_file)
        result = self.check_integrity_file(output_file)
        return result


def main():
    # Set up logging
    logging.basicConfig(
        format="%(levelname)s:%(message)s",
        level=logging.DEBUG,
        handlers=[logging.FileHandler("integrity.log"), logging.StreamHandler()],
    )
    logger = logging.getLogger(__name__)

    # Define the argument parser
    parser = argparse.ArgumentParser(
        description="""Check Integrity of big files with source one. Supports raw videos saved in yuv files.

Two modes available: stream and file.
Stream mode is for video streams saved into files segmented by time, while file mode is for single video files.
Run script with mode type and --help for more information.

Example of command:
python videointe.py stream src.yuv out_name 1920x1080 yuv422p10le --output_path /mnt/ramdisk --segment_duration 3
--workers 5

First frame of the video is determined by running ffmpeg command to extract the first frame and using OCR to read the
frame number from it.
Coordinates for the frame number extraction are set by default to X=0, Y=20, FONT=10, LENGTH=350.
You can change them by modifying the constants at the beginning of the script.""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    subparsers = parser.add_subparsers(
        dest="mode", help="Mode of operation: stream or file"
    )

    # Common arguments function to avoid repetition
    def add_common_arguments(parser):
        parser.add_argument("src", type=str, help="Path to the source video file")
        parser.add_argument("out", type=str, help="Name/prefix of the output file")
        parser.add_argument(
            "res", type=str, help="Resolution of the video (e.g., 1920x1080)"
        )
        parser.add_argument(
            "fmt", type=str, help="Format of the video file (e.g., yuv422p10le)"
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
    stream_help = """Check integrity for video stream (stream saved into files segmented by time)

It assumes that there is X digit segment number in the file name like `out_name_001.yuv` or `out_name_02.yuv`.
It can be achieved by using ffmpeg with `-f segment` option.

Example: ffmpeg -i input.yuv -f segment -segment_time 3 out_name_%03d.yuv"""
    stream_parser = subparsers.add_parser(
        "stream",
        help="Check integrity for video stream (segmented files)",
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
    stream_parser.add_argument(
        "--workers",
        type=int,
        default=5,
        help="Number of worker processes to use (default: 5)",
    )

    # File mode parser
    file_help = """Check integrity for single video file.

This mode compares a single output video file against a source reference file.
It performs frame-by-frame integrity checking using MD5 checksums."""
    file_parser = subparsers.add_parser(
        "file",
        help="Check integrity for single video file",
        description=file_help,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_common_arguments(file_parser)

    # Parse the arguments
    args = parser.parse_args()

    # Execute based on mode
    if args.mode == "stream":
        integrator = VideoStreamIntegritor(
            logger,
            args.src,
            args.out,
            args.res,
            args.fmt,
            args.output_path,
            args.segment_duration,
            args.workers,
            args.delete_file,
        )
    elif args.mode == "file":
        integrator = VideoFileIntegritor(
            logger,
            args.src,
            args.out,
            args.res,
            args.fmt,
            args.output_path,
            args.delete_file,
        )
    else:
        parser.print_help()
        return
    result = integrator.check_st20p_integrity()
    if result:
        logging.info("Integrity check passed")
    else:
        logging.error("Integrity check failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
