# MTL Test Engine

This directory contains the core components of the Media Transport Library validation test framework. The test engine provides utilities and abstractions for test execution, application management, and result reporting.

## Components

### execute.py

The `execute.py` module provides functionality for executing commands and managing processes:

- `RaisingThread`: A thread implementation that passes exceptions back to the caller
- `AsyncProcess`: Manages asynchronous process execution with output handling
- Functions for command execution with timeout and output handling

### RxTxApp.py

Provides a base class for RX/TX application interfaces used in testing:

- Application lifecycle management (start, stop, monitoring)
- Common configuration parameters for media transport applications
- Interface for test result collection and reporting

### GstreamerApp.py

GStreamer-specific application interface for testing GStreamer integration:

- Pipeline creation and management for GStreamer-based tests
- Configuration for GStreamer elements and properties
- Media processing validation utilities

### ffmpeg_app.py

FFmpeg-specific application interface for testing FFmpeg integration:

- FFmpeg command generation and execution
- Output parsing and validation
- Support for various FFmpeg encoding/decoding options

### csv_report.py

Utilities for test result reporting in CSV format:

- `csv_add_test`: Adds a test result to the report
- `csv_write_report`: Writes the report to a file
- `update_compliance_result`: Updates compliance-related results

### integrity.py

Data integrity verification tools:

- Functions to verify media data integrity after transport
- Pixel comparison and error detection
- Statistical analysis of media quality

### ramdisk.py

RAM disk management for high-performance media testing:

- `Ramdisk` class: Creates, mounts, and manages RAM disks
- Support for configurable size and mount points
- Cleanup and resource management

### const.py

Defines constants used throughout the test framework:

- Log levels and directories
- Default parameter values
- Test categorization constants

### stash.py

Provides a mechanism for storing and retrieving test data:

- Functions for stashing test results, logs, and notes
- Media file tracking and cleanup
- Issue tracking and reporting

### media_creator.py and media_files.py

Utilities for test media management:

- Media file creation for different formats and codecs
- Reference media handling for comparison tests
- Media metadata management

## Usage

The test engine components are typically used by test modules and pytest fixtures to set up test environments, execute test cases, and validate results.

Example usage in a test module:

```python
from mtl_engine.execute import run_command
from mtl_engine.RxTxApp import RxTxApp
from mtl_engine.csv_report import csv_add_test

def test_st20_rx():
    # Setup application
    app = RxTxApp(config)
    
    # Start the application
    app.start()
    
    # Run commands and validate results
    result = run_command("some_validation_command")
    
    # Add test result to report
    csv_add_test("st20_rx", result.success)
    
    # Assert test conditions
    assert result.success, "Test failed"
```

## Configuration

Most test engine components are configurable via the `test_config.yaml` and `topology_config.yaml` files in the `configs/` directory. See the main README.md for details on configuring these files.

## Extension Points

To extend the test engine with new functionality:

1. For new application types, create a subclass of `RxTxApp` with specific implementation
2. For new validation methods, add functions to `integrity.py` or create new modules
3. For new reporting formats, extend `csv_report.py` with additional report generation functions

## License

BSD-3-Clause License
Copyright (c) 2024-2025 Intel Corporation
