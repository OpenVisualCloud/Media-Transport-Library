# Media Transport Library - Integrity Testing

This directory contains tools for validating the integrity of video and audio data in the Media Transport Library.

## Overview

The integrity tools provide functionality to:

- Validate video frames using MD5 checksums and text recognition
- Validate audio frames using MD5 checksums of PCM data
- Support both file-based and stream-based (segmented files) testing

## Prerequisites

Install the required dependencies:

```bash
pip install -r requirements.txt
```

## Usage

### Audio Integrity

#### Audio File Mode

Compares a single audio file against a reference source file:

```bash
python audio_integrity.py file <source_file> <output_file> \
    --sample_size 2 --sample_num 480 --channel_num 2 \
    --output_path /path/to/output/dir
```

#### Audio Stream Mode

Checks the integrity of segmented audio files from a stream:

```bash
python audio_integrity.py stream <source_file> <segment_prefix> \
    --sample_size 2 --sample_num 480 --channel_num 2 \
    --output_path /path/to/segments/dir
```

### Video Integrity

#### Video File Mode

Compares a single video file against a reference source file:

```bash
python video_integrity.py file <source_file> <output_file> <resolution> <format> \
    --output_path /path/to/output/dir
```

#### Video Stream Mode

Checks the integrity of segmented video files from a stream:

```bash
python video_integrity.py stream <source_file> <segment_prefix> <resolution> <format> \
    --output_path /path/to/segments/dir \
    --segment_duration 3 --workers 5
```

## Integration with Test Framework

The `integrity_runner.py` provides Python classes for integrating integrity validation into test scripts:

- `FileVideoIntegrityRunner`: For single video file validation
- `StreamVideoIntegrityRunner`: For video stream validation
- `FileAudioIntegrityRunner`: For single audio file validation
- `StreamAudioIntegrityRunner`: For audio stream validation

Example usage in a test script:

```python
from common.integrity.integrity_runner import FileAudioIntegrityRunner

# Create a runner instance
runner = FileAudioIntegrityRunner(
    host=host,
    test_repo_path=repo_path,
    src_url="/path/to/source.pcm",
    out_name="output.pcm",
    sample_size=2,
    sample_num=480,
    channel_num=2,
    out_path="/mnt/ramdisk",
)

# Run the integrity check
runner.setup()
result = runner.run()
assert result, "Audio integrity check failed"
```

See the test scripts in the repository for more detailed usage examples.
