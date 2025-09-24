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

- Python 3.9 or higher
- Media Transport Library built and installed
- Test media files (currently maintained on NFS)
- Network interfaces as specified in MTL's run.md document (VFs will be created automatically)
- Root privileges or equivalent (sudo) for network operations done by script/nicctl.sh
- FFmpeg and GStreamer plugins installed (required for integration tests)

### Environment Setup

1. Create a virtual environment:

```bash
cd tests/validation
python3 -m venv venv
source venv/bin/activate
```

2. Install dependencies:

```bash
pip install -r requirements.txt
```

### Configuration

1. Update `configs/topology_config.yaml` with your network interface details:

```yaml
system:
  hostname: testserver
  interfaces:
    - name: ens801f0
      pci: "86:00.0"
      ip: "192.168.108.15"
    - name: ens801f1
      pci: "86:00.1"
      ip: "192.168.208.15"
```

2. Update `configs/test_config.yaml` with your test environment settings:

```yaml
environment:
  media_dir: "/path/to/test/media"
  log_level: "INFO"
  temp_dir: "/tmp/mtl_test"

test_params:
  timeout: 30
  retry_count: 3
  integrity_check: true
```

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

- **Network Interface Problems**: Ensure interfaces are properly configured and have the correct IP addresses
- **Permission Issues**: Many tests require root privileges for network operations
- **Media File Access**: Verify that test media files are available and accessible
- **Test Timeouts**: Increase timeout values in test_config.yaml for slower systems

### Debugging Tests

Use pytest's debug features:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -v --pdb tests/single/st20p/test_st20p_rx.py
```

Increase log verbosity:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml --log-cli-level=DEBUG tests/single/st20p/test_st20p_rx.py
```
