# Common Test Utilities

This directory contains shared utilities used across the Media Transport Library validation test suite. These utilities provide common functionality for network interface management, media integrity verification, and FFmpeg handling.

## Components

### nicctl.py

The `nicctl.py` module provides a `Nicctl` class for network interface control:

- Interface configuration and management
- PCI device binding and unbinding
- Link status monitoring
- MTU and other interface parameter configuration

Example usage:

```python
from common.nicctl import Nicctl

# Create a network interface controller
nic = Nicctl()

# Configure interface
nic.configure_interface("enp1s0f0", "192.168.1.10", "255.255.255.0")

# Check link status
status = nic.get_link_status("enp1s0f0")
```

### integrity/

This directory contains tools for verifying data integrity in media transport tests:

- Pixel comparison utilities for video integrity checks
- Audio sample verification
- Ancillary data integrity checks
- Error statistics calculation

Key modules:

- `video_integrity.py`: Functions for comparing video frames before and after transport
- `audio_integrity.py`: Functions for comparing audio samples
- `ancillary_integrity.py`: Functions for comparing ancillary data

### ffmpeg_handler/

This directory contains utilities for FFmpeg integration:

- FFmpeg command generation
- Output parsing and analysis
- Media format detection and conversion
- Encoder and decoder integration

Key modules:

- `ffmpeg_cmd.py`: Functions for generating FFmpeg command lines
- `ffmpeg_output.py`: Functions for parsing and analyzing FFmpeg output
- `ffmpeg_formats.py`: Media format definitions and utilities

### gen_frames.sh

A shell script for generating test frames for video testing:

- Creates test patterns in various formats
- Supports different resolutions and frame rates
- Configurable color patterns and test signals

## Using Common Utilities in Tests

These utilities are imported and used by test modules to set up test environments, execute tests, and validate results.

Example:

```python
from common.nicctl import Nicctl
from common.integrity.video_integrity import compare_frames

def test_st20_transport():
    # Configure network interfaces
    nic = Nicctl()
    nic.configure_interface("enp1s0f0", "192.168.1.10", "255.255.255.0")
    
    # Run transport test
    # ...
    
    # Verify frame integrity
    result = compare_frames("reference_frame.yuv", "received_frame.yuv")
    assert result.match_percentage > 99.9, "Frame integrity check failed"
```

## Extending Common Utilities

To add new common utilities:

1. Create new Python modules in the appropriate subdirectory
2. Document the module's purpose and API
3. Import the new utilities in test modules as needed

## License

BSD-3-Clause License
Copyright (c) 2024-2025 Intel Corporation
