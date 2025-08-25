#!/usr/bin/env python3

# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation
# Media Communications Mesh

import logging
import subprocess
import sys
import tempfile
from pathlib import Path


# Create mock runner classes for testing
class FileAudioIntegrityRunner:
    def __init__(
        self,
        host,
        test_repo_path,
        src_url,
        out_name,
        sample_size=2,
        sample_num=480,
        channel_num=2,
        out_path="/mnt/ramdisk",
        python_path=None,
        integrity_path=None,
        delete_file=True,
    ):
        self.host = host
        self.test_repo_path = test_repo_path
        self.src_url = src_url
        self.out_name = out_name
        self.sample_size = sample_size
        self.sample_num = sample_num
        self.channel_num = channel_num
        self.out_path = out_path
        self.delete_file = delete_file
        self.python_path = python_path or "python3"
        # Locate audio_integrity.py relative to this script's location
        if integrity_path:
            self.integrity_path = integrity_path
        else:
            # Get the directory where this script is located
            script_dir = Path(__file__).parent
            self.integrity_path = str(script_dir / "audio_integrity.py")

    def setup(self):
        logging.info(
            f"Setting up audio integrity check on {self.host.name} for {self.out_name}"
        )

    def run(self):
        cmd = " ".join(
            [
                self.python_path,
                self.integrity_path,
                "file",
                self.src_url,
                self.out_name,
                "--sample_size",
                str(self.sample_size),
                "--sample_num",
                str(self.sample_num),
                "--channel_num",
                str(self.channel_num),
                "--output_path",
                self.out_path,
                "--delete_file" if self.delete_file else "--no_delete_file",
            ]
        )
        logging.debug(
            f"Running audio integrity check on {self.host.name} for {self.out_name} with command: {cmd}"
        )
        result = self.host.connection.execute_command(
            cmd, shell=True, stderr_to_stdout=True, expected_return_codes=(0, 1)
        )
        if result.return_code > 0:
            logging.error(
                f"Audio integrity check failed on {self.host.name}: {self.out_name}"
            )
            logging.error(result.stdout)
            return False
        logging.info(
            f"Audio integrity check completed successfully on {self.host.name} for {self.out_name}"
        )
        return True


# Set up logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)


class LocalHost:
    """Simple host class for testing that mimics the expected interface"""

    def __init__(self, name="localhost"):
        self.name = name
        self.connection = self

    def path(self, *args):
        return Path(*args)

    def execute_command(
        self, cmd, shell=False, stderr_to_stdout=False, expected_return_codes=None
    ):
        class CommandResult:
            def __init__(self, return_code, stdout):
                self.return_code = return_code
                self.stdout = stdout

        logger.info(f"Executing command: {cmd}")
        try:
            result = subprocess.run(
                cmd, shell=shell, check=False, text=True, capture_output=True
            )
            logger.info(f"Command output: {result.stdout}")
            if result.stderr:
                logger.error(f"Command error: {result.stderr}")

            if expected_return_codes and result.returncode not in expected_return_codes:
                logger.error(f"Command failed with return code {result.returncode}")

            return CommandResult(
                result.returncode,
                result.stdout + result.stderr if stderr_to_stdout else result.stdout,
            )
        except Exception as e:
            logger.error(f"Exception running command: {e}")
            return CommandResult(1, str(e))


def create_test_files(test_dir, sample_size, sample_num, channel_num, frame_count):
    """Create test PCM files for the integrity test"""
    frame_size = sample_size * sample_num * channel_num

    # Create source file
    source_file = test_dir / "source.pcm"
    with open(source_file, "wb") as f:
        for i in range(frame_count):
            # Create a simple pattern for each frame
            # This example creates a pattern based on the frame number
            pattern = bytes([(i + j) % 256 for j in range(frame_size)])
            f.write(pattern)

    # Create a matching destination file
    dest_file = test_dir / "dest.pcm"
    with open(source_file, "rb") as src, open(dest_file, "wb") as dst:
        dst.write(src.read())

    # Create a corrupted file for additional testing (optional)
    corrupt_file = test_dir / "corrupt.pcm"
    with open(source_file, "rb") as src, open(corrupt_file, "wb") as dst:
        data = src.read()
        # Corrupt the data at frame 10
        frame_size = sample_size * sample_num * channel_num
        corrupt_pos = frame_size * 10
        corrupt_data = bytearray(data)
        for i in range(min(10, frame_size)):
            corrupt_data[corrupt_pos + i] = (corrupt_data[corrupt_pos + i] + 123) % 256
        dst.write(corrupt_data)

    return source_file, dest_file


def main():
    # Create temporary directory for test files
    with tempfile.TemporaryDirectory() as temp_dir:
        test_dir = Path(temp_dir)

        # Parameters for audio frames
        sample_size = 2  # 16-bit samples (2 bytes)
        sample_num = 480  # 480 samples per frame
        channel_num = 2  # stereo
        frame_count = 100  # generate 100 frames

        # Get the path to the repo
        repo_path = Path(__file__).parent.parent.parent

        # Create test files
        source_file, dest_file = create_test_files(
            test_dir, sample_size, sample_num, channel_num, frame_count
        )

        logger.info(f"Created test files in {test_dir}")
        logger.info(f"Source file: {source_file}")
        logger.info(f"Destination file: {dest_file}")

        # Create local host for testing
        host = LocalHost()

        # Test 1: Test the file audio integrity runner with valid file
        logger.info("Test 1: Testing FileAudioIntegrityRunner with valid file...")
        file_runner = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=repo_path,
            src_url=str(source_file),
            out_name=dest_file.name,
            sample_size=sample_size,
            sample_num=sample_num,
            channel_num=channel_num,
            out_path=str(test_dir),
            delete_file=False,
        )

        file_runner.setup()
        result = file_runner.run()

        if result:
            logger.info("✅ Test 1: FileAudioIntegrityRunner with valid file - PASSED")
        else:
            logger.error("❌ Test 1: FileAudioIntegrityRunner with valid file - FAILED")
            return 1

        # Test 2: Test with corrupted file (should fail)
        logger.info("Test 2: Testing FileAudioIntegrityRunner with corrupted file...")
        corrupt_runner = FileAudioIntegrityRunner(
            host=host,
            test_repo_path=repo_path,
            src_url=str(source_file),
            out_name="corrupt.pcm",
            sample_size=sample_size,
            sample_num=sample_num,
            channel_num=channel_num,
            out_path=str(test_dir),
            delete_file=False,
        )

        corrupt_runner.setup()
        corrupt_result = corrupt_runner.run()

        if not corrupt_result:
            logger.info(
                "✅ Test 2: FileAudioIntegrityRunner with corrupted file - PASSED (correctly detected corruption)"
            )
        else:
            logger.error(
                "❌ Test 2: FileAudioIntegrityRunner with corrupted file - FAILED (did not detect corruption)"
            )
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
