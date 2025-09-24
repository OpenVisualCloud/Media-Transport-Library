# MTL Validation Framework

The Media Transport Library (MTL) Validation Framework provides comprehensive testing capabilities for various aspects of the MTL, including protocol compliance, performance, and integration testing.

## Overview

The validation framework uses pytest to organize and execute tests across various scenarios, protocols, and backend implementations. It supports both automated testing in CI/CD environments and manual testing for development and troubleshooting.

## Test Framework Structure

The validation framework is organized into the following main components:

## Test Framework Structure

The validation framework is organized into the following main components:

- **common/**: Shared utilities for test functionality, including FFmpeg handlers, integrity verification tools, and network interface control
- **configs/**: Configuration files for test environment and network topology
- **mtl_engine/**: Core test framework components that manage test execution, application interfaces, and result reporting
- **tests/**: Test modules organized by scenario type:
  - **single/**: Single-flow test scenarios for various protocols (ST2110-20/22/30/40), backends, and integrations
  - **dual/**: Tests for multiple simultaneous flows
  - **invalid/**: Error handling and negative test cases

## Components Description

### Common Utilities

The `common/` directory contains shared utilities that provide fundamental functionality for test execution:

- **FFmpeg Handler**: Manages FFmpeg operations for media processing and verification
- **Integrity Tools**: Provides functions for data integrity verification between source and received media
- **Network Interface Control**: Manages network interfaces required for testing

### Configuration Files

The `configs/` directory contains YAML files that specify:

- **Test Environment Settings**: Hardware specifications, media paths, and test parameters
- **Network Topology**: Interface configuration, IP addressing, and routing information

### MTL Engine

The `mtl_engine/` directory contains the core components of the framework:

- **Execute Module**: Manages the execution flow of tests, including setup and teardown
- **Application Interfaces**: Provides interfaces to RX/TX, GStreamer, and FFmpeg applications
- **Reporting Tools**: Generates test reports and collects performance metrics

### Test Modules

The `tests/` directory contains test implementations organized by scenario type:

- **Single Flow Tests**: Tests focusing on individual protocol implementations
  - **ST2110-20**: Uncompressed video tests
  - **ST2110-22**: Compressed video tests
  - **ST2110-30**: Audio tests
  - **ST2110-40**: Ancillary data tests
  - Backend-specific tests (DMA, kernel socket, etc.)
  - Integration tests (FFmpeg, GStreamer)
  
- **Dual Flow Tests**: Tests involving multiple simultaneous flows
- **Invalid Tests**: Tests focusing on error handling and edge cases

## Setup and Installation

### Prerequisites

#### 1. Build Media Transport Library First (CRITICAL)

**⚠️ IMPORTANT**: The MTL library must be built before running validation tests!

The tests require the RxTxApp binary and other MTL components. Follow these steps:

```bash
# 1. Install build dependencies (see doc/build.md for your OS)
sudo apt-get update
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev
sudo pip install pyelftools ninja

# 2. Build DPDK (required dependency)
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v25.03
git switch -c v25.03
git am /path/to/Media-Transport-Library/patches/dpdk/25.03/*.patch
meson setup build
ninja -C build
sudo ninja install -C build
cd ..

# 3. Build MTL
cd Media-Transport-Library
./build.sh
```

For complete build instructions, see [doc/build.md](build.md).

#### 2. Other Prerequisites

- Python 3.9 or higher
- Test media files (currently maintained on NFS)
- Network interfaces as specified in MTL's run.md document (VFs will be created automatically)
- Root privileges or equivalent (sudo) for network operations done by script/nicctl.sh
- FFmpeg and GStreamer plugins installed (required for integration tests)

### Environment Setup

1. Create and activate a virtual environment:

```bash
cd tests/validation
python3 -m venv venv
source venv/bin/activate
```

**Note**: If you're using VS Code or other development tools that auto-configure Python environments, ensure you're using the correct Python interpreter. The tests require the packages from `tests/validation/requirements.txt`.

2. Install dependencies:

```bash
pip install -r requirements.txt
```

Verify installation:
```bash
python -m pytest --version
```

### Configuration

#### Critical Configuration Steps

1. **Update `configs/topology_config.yaml`** with your actual network interface details:

```yaml
---
metadata:
  version: '2.4'
hosts:
  - name: host
    instantiate: true
    role: sut
    network_interfaces:
      - pci_device: 8086:1592  # Update with your NIC's PCI device ID
        interface_index: 0
    connections:
      - ip_address: 127.0.0.1  # Use actual IP for remote hosts
        connection_type: SSHConnection
        connection_options:
          port: 22
          username: root         # Update with your username
          password: None         # Use key-based auth when possible
          key_path: /home/user/.ssh/id_rsa  # Update path to your SSH key
```

**To find your PCI device ID**: `lspci | grep Ethernet`

2. **Update `configs/test_config.yaml`** with your environment paths:

```yaml
build: /path/to/Media-Transport-Library/     # Update to your MTL root directory
mtl_path: /path/to/Media-Transport-Library/  # Update to your MTL root directory
media_path: /mnt/media                       # Update to your test media location
capture_cfg:
  enable: false    # Set to true if you want packet capture
  test_name: test_name
  pcap_dir: /mnt/ramdisk/pcap
  capture_time: 5
  interface: null  # Set to interface name if capture enabled
ramdisk:
  media: 
    mountpoint: /mnt/ramdisk/media
    size_gib: 32
  pcap:
    mountpoint: /mnt/ramdisk/pcap
    size_gib: 768
```

**Important**: 
- Set `build` and `mtl_path` to your actual MTL installation directory
- Set `media_path` to where your test media files are located
- Ensure the paths exist and are accessible

## Running Tests

### Basic Usage

Run all tests:

```bash
cd tests/validation
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml
```

Run smoke tests:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke
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

- `@pytest.mark.smoke`: Basic functionality tests for quick validation
- `@pytest.mark.nightly`: Comprehensive tests for nightly runs
- `@pytest.mark.performance`: Performance benchmarking tests
- `@pytest.mark.dma`: Tests specific to DMA functionality
- `@pytest.mark.fwd`: Tests for packet forwarding
- `@pytest.mark.kernel_socket`: Tests for kernel socket backend
- `@pytest.mark.xdp`: Tests for XDP backend
- `@pytest.mark.gpu`: Tests involving GPU processing

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
- Test results are displayed in the console
- HTML reports can be generated as described above
- CSV reports can be generated for performance benchmarks

## Extending the Framework

### Adding New Tests

1. Create a new test file in the appropriate directory under `tests/`
2. Import the required fixtures from `conftest.py`
3. Implement test functions using pytest conventions
4. Add appropriate markers for test categorization

Example:

```python
import pytest
from mtl_engine.RxTxApp import RxTxApp

@pytest.mark.smoke
@pytest.mark.st20p
def test_st20p_basic_flow(setup_interfaces, media_files):
    """Test basic ST2110-20 flow from TX to RX"""
    app = RxTxApp(setup_interfaces)
    
    # Test implementation
    result = app.run_st20p_test(media_files["1080p"])
    
    # Assertions
    assert result.success, "ST2110-20 flow test failed"
    assert result.packet_loss == 0, "Packet loss detected"
```

### Adding New Functionality

To add new functionality to the framework:

1. Add utility functions in the appropriate module under `common/`
2. Update the relevant application interface in `mtl_engine/`
3. Document the new functionality in code comments
4. Add tests that exercise the new functionality

## Troubleshooting

### Common Issues

#### RxTxApp Command Not Found
**Error**: `sudo: ./tests/tools/RxTxApp/build/RxTxApp: command not found`
**Solution**: The MTL library hasn't been built yet. Follow the build instructions in the Prerequisites section above or see [doc/build.md](build.md).

#### Virtual Environment Issues
**Problem**: Package installation conflicts or wrong Python interpreter
**Solution**: 
```bash
# Remove existing venv and recreate
rm -rf venv
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

#### Configuration File Issues
**Problem**: Tests fail with connection or path errors
**Solution**: 
- Verify `configs/test_config.yaml` has correct paths (especially `build` and `mtl_path`)
- Update `configs/topology_config.yaml` with actual network interface details
- Use `lspci | grep Ethernet` to find your PCI device IDs

#### Network Interface Problems
**Problem**: Interface configuration errors
**Solution**: Ensure interfaces are properly configured and have the correct IP addresses

#### Permission Issues
**Problem**: Network operation failures
**Solution**: Many tests require root privileges for network operations. Run with appropriate sudo permissions.

#### Media File Access
**Problem**: Media files not found
**Solution**: Verify that test media files are available and accessible at the path specified in `media_path`

#### Test Timeouts
**Problem**: Tests timing out on slower systems
**Solution**: Increase timeout values in test_config.yaml for slower systems

### Debugging Tests

Use pytest's debug features:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -v --pdb tests/single/st20p/test_st20p_rx.py
```

Increase log verbosity:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml --log-cli-level=DEBUG tests/single/st20p/test_st20p_rx.py
```
