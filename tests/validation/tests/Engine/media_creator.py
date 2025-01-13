import os
import subprocess
import time

from tests.Engine.execute import log_fail, log_info


def create_video_file(
    width: int,
    height: int,
    framerate: str,
    format: str,
    media_path: str,
    output_path: str = "test.yuv",
    pattern: str = "ball",
    duration: int = 10,
):
    file_path = os.path.join(media_path, output_path)
    command = [
        "gst-launch-1.0",
        "-v",
        "videotestsrc",
        f"pattern={pattern}",
        "!",
        f"video/x-raw,width={width},height={height},format={format},framerate={framerate}/1",
        "!",
        "timeoverlay",
        "!",
        "videoconvert",
        "!",
        "filesink",
        f"location={file_path}",
    ]

    log_info(f"Creating video file with command: {' '.join(command)}")

    process = subprocess.Popen(command)

    try:
        time.sleep(duration)
        process.terminate()
        process.wait()
        log_info(f"Video file created at {file_path}")
    except subprocess.SubprocessError as e:
        log_fail(f"Failed to create video file: {e}")
        raise

    return file_path


def create_audio_file_sox(
    sample_rate: int,
    channels: int,
    bit_depth: int,
    frequency: int = 440,
    output_path: str = "test.pcm",
    duration: int = 10,
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

    log_info(f"Creating audio file with command: {' '.join(command)}")

    try:
        subprocess.run(command, check=True)
        log_info(f"Audio file created at {output_path}")
    except subprocess.CalledProcessError as e:
        log_fail(f"Failed to create audio file: {e}")
        raise


def remove_file(file_path: str):
    if os.path.exists(file_path):
        os.remove(file_path)
        log_info(f"Removed file: {file_path}")
    else:
        log_info(f"File not found: {file_path}")
