import os
import re
import subprocess

from tests.Engine.execute import log_fail, LOG_FOLDER


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
