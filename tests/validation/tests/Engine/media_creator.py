import os
import subprocess
import logging
import time

def create_video_file(
    width: int,
    height: int,
    framerate: str,
    format: str,
    media_path: str,
    output_path: str = "test.yuv",
    pattern: str = 'ball',
    duration: int = 10
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
        f"location={file_path}"
    ]

    logging.info(f"Creating video file with command: {' '.join(command)}")

    process = subprocess.Popen(command)

    try:
        time.sleep(duration)
        process.terminate()
        process.wait()
        logging.info(f"Video file created at {file_path}")
    except subprocess.SubprocessError as e:
        logging.error(f"Failed to create video file: {e}")
        raise

    return file_path

def remove_file(file_path: str):
    if os.path.exists(file_path):
        os.remove(file_path)
        logging.info(f"Removed file: {file_path}")
    else:
        logging.warning(f"File not found: {file_path}")