import os
import re
import subprocess

from tests.Engine.execute import LOG_FOLDER, log_fail


def check_st20p_integrity(src_url: str, out_url: str, size: str):
    logs_dir = os.path.join(os.getcwd(), LOG_FOLDER, "latest")

    out_sums_file = os.path.join(logs_dir, "out_sums.txt")
    subprocess.run(
        ["ffmpeg", "-s", size, "-i", out_url, "-f", "framemd5", out_sums_file]
    )

    src_sums_file = os.path.join(logs_dir, "src_sums.txt")
    subprocess.run(
        ["ffmpeg", "-s", size, "-i", src_url, "-f", "framemd5", src_sums_file]
    )

    sum_pattern = re.compile(r"[0-9a-f]{32}")

    src_sums = []

    with open(src_sums_file, "r") as f:
        for line in f:
            match = sum_pattern.search(line)

            if match:
                src_sums.append(match.group(0))

    out_sums = []

    with open(out_sums_file, "r") as f:
        for line in f:
            match = sum_pattern.search(line)

            if match:
                out_sums.append(match.group(0))

    if len(src_sums) < len(out_sums):
        # received file is longer, so it contains all frames from sended
        # iterating over sended frames

        for i in range(len(src_sums)):
            if src_sums[i] != out_sums[i]:
                log_fail(
                    f"Checksum of frame {i} is invalid. Sent {src_sums[i]}, received {out_sums[i]}"
                )
                return False
    else:
        # received file is shorter (so it contains part of sended frames) or equal to sended file
        # iterating over received frames

        for i in range(len(out_sums)):
            if out_sums[i] != src_sums[i]:
                log_fail(
                    f"Checksum of frame {i} is invalid. Sent {src_sums[i]}, received {out_sums[i]}"
                )
                return False

    return True


def check_st30p_integrity(src_url: str, out_url: str):
    src_chunks = []
    frame_size = 1  # Placeholder! TODO Calculate actual frame size

    with open(src_url, "rb") as f:
        while chunk := f.read(frame_size):
            src_chunks.append(chunk)

    out_chunks = []

    with open(out_url, "rb") as f:
        while chunk := f.read(frame_size):
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
