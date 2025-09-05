# Media Transport Library Validation Test Suite

This directory contains the automated validation test suite for the Media Transport Library. The tests are designed to verify the functionality, performance, and compliance of the Media Transport Library with SMPTE ST2110 standards.

## Overview

The validation framework uses pytest to organize and execute tests across various components of the Media Transport Library. It supports testing of single and dual flow scenarios, various transport protocols, and integration with media processing tools like FFmpeg and GStreamer.

## Test Framework Structure

```
tests/validation/
├── common/              # Shared utilities for tests
│   ├── ffmpeg_handler/  # FFmpeg integration utilities
│   ├── integrity/       # Data integrity verification tools
│   └── nicctl.py        # Network interface control
├── configs/             # Test configuration files
│   ├── test_config.yaml      # Test environment settings
│   └── topology_config.yaml  # Network topology configuration
├── create_pcap_file/    # Tools for packet capture file creation
├── mtl_engine/          # Core test framework components
│   ├── execute.py       # Test execution management
│   ├── RxTxApp.py       # RX/TX application interface
│   ├── GstreamerApp.py  # GStreamer integration
│   ├── ffmpeg_app.py    # FFmpeg integration
│   ├── csv_report.py    # Test result reporting
│   └── ramdisk.py       # RAM disk management
├── tests/               # Test modules
│   ├── single/          # Single-flow test scenarios
│   │   ├── dma/         # DMA tests
│   │   ├── ffmpeg/      # FFmpeg integration tests
│   │   ├── gstreamer/   # GStreamer integration tests
│   │   ├── kernel_socket/ # Kernel socket tests
│   │   ├── performance/ # Performance benchmarking
│   │   ├── ptp/         # Precision Time Protocol tests
│   │   ├── st20p/       # ST2110-20 video tests
│   │   ├── st22p/       # ST2110-22 compressed video tests
│   │   ├── st30p/       # ST2110-30 audio tests
│   │   └── st41/        # ST2110-40 ancillary data tests
│   ├── dual/            # Dual-flow test scenarios
│   └── invalid/         # Error handling and negative test cases
├── conftest.py          # pytest configuration and fixtures
├── pytest.ini           # pytest settings
└── requirements.txt     # Python dependencies
```

## Setup and Installation

### Prerequisites

- Python 3.9 or higher
- Media Transport Library built and installed
- Network interfaces configured for testing
- Sufficient permissions for network management

### Environment Setup

1. Create and activate a Python virtual environment:

```bash
python -m venv venv
source venv/bin/activate
```

2. Install required dependencies:

```bash
pip install -r requirements.txt
```

3. Configure test parameters:

Edit `configs/test_config.yaml` with the appropriate paths:
- Set `build` and `mtl_path` to the path of your Media Transport Library build
- Configure `media_path` to point to your test media files
- Adjust RAM disk settings if needed

Edit `configs/topology_config.yaml` to match your network configuration:
- Set the correct `ip_address`, `SSH_PORT`, `USERNAME`, and either use `KEY_PATH` 
- Configure the appropriate `pci_device` for your network interfaces

4. Start the MtlManager service:

```bash
sudo MtlManager &
```

5. (Optional) Create VFs for NIC testing:

```bash
sudo ./script/nicctl.sh create_vf "${TEST_PF_PORT_P}"
sudo ./script/nicctl.sh create_vf "${TEST_PF_PORT_R}"
```

Replace `${TEST_PF_PORT_P}` and `${TEST_PF_PORT_R}` with your physical port identifiers.

## Running Tests

### Basic Test Execution

Run all tests with configuration files:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml
```

Run specific test modules:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml tests/single/st20p/test_st20p_rx.py
```

### Test Categories

The tests are categorized with markers that can be used to run specific test groups:

- **Smoke Tests**: Quick verification tests
  ```bash
  python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke
  ```

- **Nightly Tests**: Comprehensive tests suitable for nightly runs
  ```bash
  python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m nightly
  ```

### Generating HTML Reports

You can generate HTML reports for test results:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke --template=html/index.html --report=report.html
```

### Test Output and Reports

- Logs are written to `pytest.log`
- CSV reports are generated for compliance results
- The framework stores test results in a structured format for later analysis

## Test Configuration

### RAM Disk Configuration

Tests utilize RAM disks for high-performance media handling. Configure in `test_config.yaml`:

```yaml
ramdisk:
  media: 
    mountpoint: /mnt/ramdisk/media
    size_gib: 32
  pcap:
    mountpoint: /mnt/ramdisk/pcap
    size_gib: 768
```

### Network Capture

Configure network packet capture settings in `test_config.yaml`:

```yaml
capture_cfg:
  enable: true
  test_name: test_name
  pcap_dir: /mnt/ramdisk/pcap
  capture_time: 5
  interface: enp1s0f0
```

## Test Types

### Media Flow Tests

- **ST20p**: Tests for ST2110-20 (uncompressed video)
- **ST22p**: Tests for ST2110-22 (compressed video)
- **ST30p**: Tests for ST2110-30 (audio)
- **ST41**: Tests for ST2110-40 (ancillary data)

### Backend Tests

- **DMA**: Direct Memory Access tests
- **Kernel Socket**: Tests for kernel socket backend
- **XDP**: Tests for Express Data Path backend

### Integration Tests

- **FFmpeg**: Tests for FFmpeg integration
- **GStreamer**: Tests for GStreamer integration

### Performance Tests

- Tests to measure throughput, latency, and other performance metrics

## Extending the Test Suite

### Adding New Tests

1. Create a new test file in the appropriate directory under `tests/`
2. Follow the pytest format for test functions
3. Use existing fixtures from `conftest.py` or create new ones as needed
4. Add appropriate markers for test categorization

### Adding New Test Categories

1. Define the new marker in `pytest.ini`
2. Create a new directory under `tests/` if necessary
3. Add test files with the new marker

## Troubleshooting

### Common Issues

- **Network Interface Not Found**: Verify the interface configuration in `topology_config.yaml`
- **Test Media Not Found**: Check the `media_path` setting in `test_config.yaml`
- **Permission Issues**: Ensure the user has sufficient permissions for network operations

### Logs and Debugging

- Check `pytest.log` for detailed test execution logs
- Use the `--verbose` flag for more detailed output
- For network issues, use the packet capture feature to analyze traffic

## License

BSD-3-Clause License
Copyright (c) 2024-2025 Intel Corporation
