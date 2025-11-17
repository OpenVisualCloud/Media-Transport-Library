# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import logging
import os
import random
import string
import subprocess
import threading
import time

from .execute import log_fail, run

logger = logging.getLogger(__name__)


def _terminate_gst_process_after_duration(duration: int, host):
    """Terminate gstreamer process after specified duration"""
    time.sleep(duration)
    # Find and terminate the gst-launch process on remote host
    terminate_cmd = "pkill -SIGINT gst-launch-1.0"
    try:
        run(terminate_cmd, host=host)
        logger.info("Sent SIGINT to gst-launch process")
    except Exception:
        # Fallback to SIGTERM
        terminate_cmd = "pkill -SIGTERM gst-launch-1.0"
        try:
            run(terminate_cmd, host=host)
            logger.info("Sent SIGTERM to gst-launch process")
        except Exception:
            pass


def create_video_file(
    width: int,
    height: int,
    framerate: str,
    format: str,
    media_path: str,
    output_path: str = "test_video.yuv",
    pattern: str = "ball",
    duration: int = 3,
    host=None,
):
    file_path = os.path.join(media_path, output_path)
    if "/" not in framerate:
        framerate += "/1"

    command = [
        "gst-launch-1.0",
        "-v",
        "videotestsrc",
        f"pattern={pattern}",
        "!",
        f"video/x-raw,width={width},height={height},format={format},framerate={framerate}",
        "!",
        "timeoverlay",
        "!",
        "videoconvert",
        "!",
        "filesink",
        f"location={file_path}",
    ]

    logger.info(f"Creating video file with command: {' '.join(command)}")

    try:
        # Remote execution - start process and terminate after duration
        # Start the termination thread
        termination_thread = threading.Thread(
            target=_terminate_gst_process_after_duration, args=(duration, host)
        )
        termination_thread.daemon = True
        termination_thread.start()

        # Run the gstreamer command (this will block until process is terminated)
        try:
            result = run(" ".join(command), host=host)
            # Wait for termination thread to complete
            termination_thread.join(timeout=1)

            # Check if process was terminated by our signal (expected behavior)
            # Handle both subprocess and SSH process objects
            return_code = getattr(result, "returncode", getattr(result, "exit_code", 0))
            if return_code in [
                0,
                -2,
                -15,
                130,
            ]:  # 0=success, -2=SIGINT, -15=SIGTERM, 130=SIGINT in bash
                logger.info("Process terminated successfully after duration")
            else:
                logger.info(f"Process ended with return code {return_code}")
        except Exception:
            # Ensure termination thread completes
            termination_thread.join(timeout=1)
            raise

        logger.info(f"Video file created at {file_path}")
    except Exception as e:
        log_fail(f"Failed to create video file: {e}")
        raise

    return file_path


def create_audio_file_sox(
    sample_rate: int,
    channels: int,
    bit_depth: int,
    frequency: int = 440,
    output_path: str = "test_audio.pcm",
    duration: int = 10,
    host=None,
):
    """
    Create an audio file with the provided arguments using sox.
    """
    command = [
        "sox",
        "-n",
        "-r",
        str(sample_rate),
        "-c",
        str(channels),
        "-b",
        str(bit_depth),
        "-t",
        "raw",
        output_path,
        "synth",
        str(duration),
        "sine",
        str(frequency),
    ]

    logger.info(f"Creating audio file with command: {' '.join(command)}")

    try:
        result = run(" ".join(command), host=host)
        return_code = getattr(result, "returncode", getattr(result, "exit_code", 0))
        if return_code == 0:
            logger.info(f"Audio file created at {output_path}")
        else:
            raise subprocess.CalledProcessError(return_code, command)
    except Exception as e:
        log_fail(f"Failed to create audio file: {e}")
        raise


def create_text_file(size_kb: int, output_path: str = "test_anc.txt", host=None):
    size_bytes = size_kb * 1024
    chars = string.ascii_letters + string.digits + string.punctuation

    # Remote file creation
    content = ""
    while size_bytes > 0:
        chunk_size = min(size_bytes, 1024)
        content += "".join(random.choice(chars) for _ in range(chunk_size))
        size_bytes -= chunk_size

    # Escape content for shell command and use printf for better handling
    # Split into smaller chunks to avoid command line length limits
    chunk_size = 8192  # 8KB chunks
    command_parts = []
    for i in range(0, len(content), chunk_size):
        chunk = content[i : i + chunk_size]
        escaped_chunk = chunk.replace("\\", "\\\\").replace("'", "'\"'\"'")
        if i == 0:
            command_parts.append(f"printf '%s' '{escaped_chunk}' > {output_path}")
        else:
            command_parts.append(f"printf '%s' '{escaped_chunk}' >> {output_path}")

    try:
        for cmd in command_parts:
            result = run(cmd, host=host)
            return_code = getattr(result, "returncode", getattr(result, "exit_code", 0))
            if return_code != 0:
                raise subprocess.SubprocessError(
                    f"Command failed with return code {return_code}"
                )
    except Exception as e:
        log_fail(f"Failed to create text file: {e}")
        raise

    logger.info(f"Text file created at {output_path} with size {size_kb} KB")
    return output_path


# Pseudo rfc8331 file does iterate over
def create_ancillary_rfc8331_pseudo_file(
    size_frames: int, output_path: str = "test_anc.txt", host=None
):
    # Define a table of 3 ancillary hex inputs (as lists of hex strings)
    # Example ancillary data table (each entry is a bytes object)
    # 1st block: 3 packets:
    #   1: C=0b0, Line=9, HOffset=0, S=0b0, DID=0x60, SDID=0x60 (Time Code S12M-2), Stream=0, DataCount=16
    #   2: C=0b0, Line=10, HOffset=0, S=0b0, DID=0x61, SDID=0x01 (EIA 708B S334-1), Stream=0, DataCount=16
    #   3: C=0b0, Line=11, HOffset=0, S=0b0, DID=0x41, SDID=0x07 (SCTE 104 S2010), Stream=0, DataCount=60
    # 2nd block: 1 packet:
    #   1: C=0b0, Line=9, HOffset=0, S=0b0, DID=0x60, SDID=0x60 (Time Code S12M-2), Stream=0, DataCount=8
    # 3rd block: 3 packets:
    #   1: C=0b0, Line=9, HOffset=0, S=0b0, DID=0x60, SDID=0x01 (EIA 708B S334-1), Stream=0, DataCount=16
    #   2: C=0b0, Line=10, HOffset=0, S=0b0, DID=0x60, SDID=0x60 (Time Code S12M-2), Stream=0, DataCount=16
    #   3: C=0b0, Line=11, HOffset=0, S=0b0, DID=0x41, SDID=0x07 (SCTE 104 S2010), Stream=0, DataCount=60
    # 4rd block: 0 packets:
    ancillary_hex_table = [
        "\\x00\\x09\\x00\\x00\\x00\\x60\\x60\\x10\\x70\\x50\\x50\\x00\\x20\\x80\\x00\\x00"
        "\\x40\\x50\\x50\\x20\\x10\\x00\\xe0\\x00\\x00\\x0a\\x00\\x00\\x00\\x61\\x01\\x10"
        "\\x96\\x69\\x10\\x7f\\x43\\x3c\\x6a\\x72\\xe1\\xfd\\x80\\x80\\x74\\x3c\\x6a\\x1f"
        "\\x00\\x0e\\x00\\x00\\x00\\x41\\x07\\x3c\\x08\\xff\\xff\\x00\\x3c\\x00\\x00\\x00"
        "\\x00\\x00\\x00\\x00\\x03\\x01\\x04\\x00\\x02\\x12\\xf8\\x01\\x09\\x00\\x06\\x14"
        "\\x04\\x23\\x2a\\x30\\x32\\x01\\x0b\\x00\\x1b\\x00\\x00\\x03\\xe9\\x00\\xa4\\x5b"
        "\\x01\\x09\\x31\\x31\\x38\\x31\\x32\\x38\\x33\\x31\\x32\\x22\\x01\\x01\\x00\\x01"
        "\\x00\\x00\\x00\\x00",
        "\\x00\\x09\\x00\\x00\\x00\\x60\\x60\\x08\\x44\\x00\\x00\\x00\\x00\\x00\\x00\\x00",
        "\\x00\\x09\\x00\\x00\\x00\\x61\\x01\\x10\\x96\\x69\\x10\\x7f\\x43\\x5d\\xbc\\x72"
        "\\xe1\\xfd\\x01\\x85\\x74\\x5d\\xbc\\xb3\\x00\\x0a\\x00\\x00\\x00\\x60\\x60\\x10"
        "\\x70\\x20\\x60\\x10\\x20\\x80\\x50\\x00\\x50\\x50\\x20\\x20\\x00\\x00\\xe0\\x00"
        "\\x00\\x0c\\x00\\x00\\x00\\x41\\x07\\x3c\\x08\\xff\\xff\\x00\\x3c\\x00\\x00\\x00"
        "\\x00\\x00\\x00\\x00\\x03\\x01\\x04\\x00\\x02\\x12\\xf8\\x01\\x09\\x00\\x06\\x14"
        "\\x04\\x23\\x2a\\x30\\x32\\x01\\x0b\\x00\\x1b\\x00\\x00\\x03\\xe9\\x00\\x01\\x0e"
        "\\x01\\x09\\x31\\x31\\x38\\x31\\x33\\x38\\x37\\x38\\x31\\x22\\x01\\x01\\x00\\x01"
        "\\x00\\x00\\x00\\x00",
        "",
    ]

    # Generate content by repeating ancillary_hex_table entries for each frame
    content = ""
    for i in range(size_frames):
        entry = ancillary_hex_table[i % len(ancillary_hex_table)]
        content += entry

    chunk_size = 8192  # 8KB chunks
    command_parts = []
    for i in range(0, len(content), chunk_size):
        chunk = content[i : i + chunk_size]
        escaped_chunk = chunk.replace("\\", "\\\\").replace("'", "'\"'\"'")
        if i == 0:
            command_parts.append(f"printf {escaped_chunk} > {output_path}")
        else:
            command_parts.append(f"printf {escaped_chunk} >> {output_path}")

    try:
        for cmd in command_parts:
            result = run(cmd, host=host)
            logging.info(f"{cmd}")
            return_code = getattr(result, "returncode", getattr(result, "exit_code", 0))
            if return_code != 0:
                raise subprocess.SubprocessError(
                    f"Command failed with return code {return_code}"
                )
    except Exception as e:
        log_fail(f"Failed to create text file: {e}")
        raise

    size_kb = len(content) / 1024
    logger.info(f"Text file created at {output_path} with size {size_kb} KB")
    return output_path


def remove_file(file_path: str, host=None):
    # Always attempt to remove the file
    command = f"rm -f {file_path}"
    try:
        run(command, host=host)

        # Check if file still exists after removal attempt
        check_cmd = f"test -f {file_path}"
        result = run(check_cmd, host=host)
        return_code = getattr(result, "returncode", getattr(result, "exit_code", 0))

        if return_code != 0:
            # File does not exist (removal successful or file wasn't there)
            logger.info(f"Removed file: {file_path}")
        else:
            # File still exists (removal failed)
            logger.info(f"Failed to remove file: {file_path}")
    except Exception as e:
        log_fail(f"Failed to remove file: {e}")
