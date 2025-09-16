# Media Transport Library Validation Test Suite

This directory contains the automated validation test suite for the Media Transport Library. The tests are designed to verify the functionality, performance, and compliance of the Media Transport Library with SMPTE ST2110 standards.

## Overview

The validation framework uses pytest to organize and execute tests across various components of the Media Transport Library. It supports testing of single and dual flow scenarios, various transport protocols, and integration with media processing tools like FFmpeg and GStreamer.

## Test Framework Structure

The validation framework is organized into the following main components:

- **common/**: Shared utilities for test functionality, including FFmpeg handlers, integrity verification tools, and network interface control
- **configs/**: Configuration files for test environment and network topology
- **mtl_engine/**: Core test framework components that manage test execution, application interfaces, and result reporting
- **tests/**: Test modules organized by scenario type:
  - **single/**: Single-flow test scenarios for various protocols (ST2110-20/22/30/40), backends, and integrations
  - **dual/**: Tests for multiple simultaneous flows
  - **invalid/**: Error handling and negative test cases

## Setup and Installation

### Prerequisites

- Python 3.9 or higher
- Media Transport Library built and installed
- Test media files (currently maintained on NFS)
- Network interfaces as specified in MTL's run.md document (VFs will be created automatically)
- Root privileges or equivalent (sudo) for network operations done by script/nicctl.sh
- FFmpeg and GStreamer plugins installed (required for integration tests)

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
- Set the correct `ip_address`, `SSH_PORT`, `USERNAME`, and `KEY_PATH`
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

Run specific test cases with parameters:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]"
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

You can generate comprehensive HTML reports for test results that include test status, execution time, and detailed logs:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke --template=html/index.html --report=report.html
```

The generated report (report.html) provides:
- Test execution summary and statistics
- Detailed pass/fail status for each test
- Execution time and performance metrics
- Error logs and tracebacks for failed tests
- System information for better debugging context

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
