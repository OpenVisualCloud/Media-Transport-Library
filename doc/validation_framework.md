# MTL Validation Framework

The Media Transport Library (MTL) Validation Framework provides comprehensive testing capabilities for various aspects of the MTL, including protocol compliance, performance, and integration testing.

## Documentation Navigation

🚀 **Quick Setup**: [Validation Quick Start Guide](validation_quickstart.md) - Get running in 3 steps  
📁 **Local README**: [tests/validation/README.md](../tests/validation/README.md) - Quick reference and test categories  
🔧 **Build Guide**: [build.md](build.md) - MTL build instructions  

---

## Overview

The validation framework uses pytest to organize and execute tests across various scenarios, protocols, and backend implementations. It supports both automated testing in CI/CD environments and manual testing for development and troubleshooting.

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

#### gen_frames.sh

A shell script for generating test frames for video testing:

- Creates test patterns in various formats
- Supports different resolutions and frame rates  
- Configurable color patterns and test signals
- Generates files like `ParkJoy_1080p.yuv`, test patterns, and various resolution formats

**Prerequisites**: Requires FFmpeg with text filters enabled.

**Usage**:
```bash
cd tests/validation/common  # Must be in this directory
./gen_frames.sh  # Generates test media files for validation
# Generated files will be available for test configuration
```

**Troubleshooting**: If you get "No such filter: 'drawtext'" errors, install a complete FFmpeg build or skip media generation.

#### RxTxApp Test Tool

**CRITICAL**: Tests require the RxTxApp tool which is not built by the main MTL build process.

**Build Instructions** (required before running tests):
```bash
cd tests/tools/RxTxApp
meson setup build
meson compile -C build
cd ../../..
```

**Location**: After building, RxTxApp is available at `tests/tools/RxTxApp/build/RxTxApp`

**Supported Formats**:
- Resolutions: 3840x2160, 1920x1080, 1280x720, 640x360
- Pixel formats: yuv422p, yuv422p10le
- Custom color patterns and test signals with timestamps
- Configurable frame rates and durations

### Configuration Files

The `configs/` directory contains YAML files that specify:

- **Test Environment Settings**: Hardware specifications, media paths, and test parameters
- **Network Topology**: Interface configuration, IP addressing, and routing information

#### [`test_config.yaml`](../tests/validation/configs/test_config.yaml)

Location: `tests/validation/configs/test_config.yaml`

Defines the test execution environment:

**Key Parameters**:
- **build**: Path to MTL build directory
- **mtl_path**: Path to MTL installation directory
- **media_path**: Path to test media files directory
- **ramdisk.media.mountpoint**: Mount point for media RAM disk
- **ramdisk.media.size_gib**: Size of media RAM disk in GiB
- **ramdisk.pcap.mountpoint**: Mount point for packet capture RAM disk
- **ramdisk.pcap.size_gib**: Size of packet capture RAM disk in GiB

#### [`topology_config.yaml`](../tests/validation/configs/topology_config.yaml)

Location: `tests/validation/configs/topology_config.yaml`

Defines the network topology and host configuration.

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
    - Progressive and interlaced GStreamer loops (RFC8331, fps/frame buffer sweeps)
    - Split-mode packetized ANC with frame-info logging (sequence discontinuity, packet totals, RTP marker) and ring-size validation
    - Pacing sanity via RTP sender helpers and ramdisk-backed media fixtures (configure `ramdisk.media` in `configs/test_config.yaml`)
    - Redundant ST40p/ST40i GStreamer ANC cases with per-port seq-gap scheduling (real payloads, lifted packet caps) and frame-info checks for seq discontinuity/loss logging
    - Interlace auto-detect on RX (enabled by default); frame-info includes `second_field` (bool) and `interlaced` for detected cadence, and detection resets on `seq_discont` before re-learning from subsequent F bits
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

# 4. Install MTL system-wide (REQUIRED for RxTxApp)
sudo ninja install -C build
sudo ldconfig

# 5. Build required test tools (CRITICAL for validation)
cd tests/tools/RxTxApp
meson setup build
meson compile -C build
cd ../../..
```

> **⚠️ CRITICAL**:
> - The RxTxApp tool is required for validation tests but not built by the main build process
> - RxTxApp requires MTL to be installed system-wide to build successfully
> - You must build it separately after installing MTL

For complete build instructions, see [doc/build.md](build.md).

#### 2. Other Prerequisites

- **Python 3.9 or higher**
- **Test Media Files**: Input data files required for testing
  - Test media files are necessary for running video, audio, and ancillary data tests
  - These files are currently maintained on NFS in production environments
  - For local testing, you can generate test frames using `tests/validation/common/gen_frames.sh` (see [gen_frames.sh section](#gen_framessh))
  - Configure the media file location in `configs/test_config.yaml` using the `media_path` parameter
- **Network Interfaces**: Configure interfaces according to MTL's [run.md](run.md) documentation
  - Basic MTL network setup must be completed as described in run.md
  - Virtual Functions (VFs) will be created automatically by the validation framework
  - No manual VF creation is required
- **Root User Privileges**: MTL validation framework must run as root user
  - Required for network management operations performed by `script/nicctl.sh`
  - Direct network interface manipulation requires root access
  - No alternative permission model is currently supported
  - Use `sudo` with the full path to your virtual environment Python (e.g., `sudo ./venv/bin/python3`)
- **FFmpeg and GStreamer Plugins**: Required for integration tests
  - Install FFmpeg: `sudo apt-get install ffmpeg`
  - Install GStreamer and plugins: `sudo apt-get install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad`
  - Some tests will fail if these are not installed

### Environment Setup

> **🚀 Quick Setup**: See [Validation Quick Start Guide](validation_quickstart.md) for streamlined setup steps.

For detailed setup:

1. Create Python virtual environment in `tests/validation/`:

```bash
cd tests/validation
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
pip install -r common/integrity/requirements.txt
```

### Configuration

#### Critical Configuration Steps

1. **Update [`configs/topology_config.yaml`](../tests/validation/configs/topology_config.yaml)** with your actual network interface details:

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
          username: root         # ⚠️ MUST be root for MTL validation
          password: None         # Use key-based auth when possible
          key_path: /root/.ssh/id_rsa  # Update path to your SSH key
```

**Device Specification Options**:
You can specify network devices in multiple ways:
- **PCI device ID** (recommended): `"0000:18:00.0"` (find with `lspci | grep Ethernet`)
- **Interface name**: `"enp175s0f0np0"` (find with `ip link show`)
- **System name**: Use your actual system hostname in the `name` field for the host
- **Environment variables**: `"${TEST_PF_PORT_P}"` (if you set them)

**To find your device options**:
```bash
# Find PCI device IDs
lspci | grep Ethernet

# Find system interface names  
ip link show
```

2. **Update [`configs/test_config.yaml`](../tests/validation/configs/test_config.yaml)** with your environment paths:

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

GStreamer ST2110-40 ancillary tests use the `media_file` fixture, which creates inputs/outputs on `ramdisk.media.mountpoint`; mount and size this ramdisk before running the suite to avoid `/tmp` spills.

#### Optional: Create VFs for Advanced Testing

For NIC testing with Virtual Functions:

```bash
# First, identify your network devices
lspci | grep Ethernet

# Create VFs (replace with your actual PCI device IDs or interface names)
sudo ./script/nicctl.sh create_vf "0000:18:00.0"  # Replace with your primary port
sudo ./script/nicctl.sh create_vf "0000:18:00.1"  # Replace with your secondary port
```

**Examples of valid identifiers**:
- PCI device ID: `"0000:18:00.0"`
- Interface name: `"enp24s0f0"`
- Environment variables: `"${TEST_PF_PORT_P}"` (if you set them)

## Running Tests

> **⚠️ CRITICAL**: Tests must be run as **root user**, not regular user. MTL validation framework requires root privileges for network operations.### Basic Test Execution

### Run specific test with parameters

**Examples of running tests with specific parameters**:
```bash
# Run fps test with specific parameters
pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]"

# Run specific integrity test with resolution parameters
pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/dual/st20p/integrity/test_integrity.py::test_integrity[yuv422p10le-1920x1080]"

# Run specific packing test
pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/dual/st20p/packing/test_packing.py::test_packing[bpm-10]"

# Run audio format test with specific format
pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/dual/st30p/st30p_format/test_st30p_format.py::test_st30p_format[pcm24]"
```

> **🚀 Quick Test Execution**: See [Quick Start Guide](validation_quickstart.md#3-run-tests) for basic test commands.

For comprehensive test execution:

### Running Specific Tests with Parameters

Run a specific test case with custom parameters:

```bash
pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]"
```

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke
```

Run specific test modules:

```bash
sudo ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml tests/single/st20p/test_st20p_rx.py
```

Run specific test cases with parameters:

```bash
sudo ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]"
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
- `@pytest.mark.verified`: Tests passing the [Verified criteria](#verified-test-criteria)

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

#### Build and Setup Issues

**Problem**: `RxTxApp: command not found`
**Solution**: Build the RxTxApp test tool separately:
```bash
cd tests/tools/RxTxApp
meson setup build
meson compile -C build
cd ../../..
```

**Problem**: RxTxApp build fails with "ST20P_TX_FLAG_EXACT_USER_PACING undeclared" or other header errors
**Solution**: Install MTL system-wide before building RxTxApp:
```bash
cd /path/to/Media-Transport-Library
sudo ninja install -C build
sudo ldconfig
# Then build RxTxApp
cd tests/tools/RxTxApp
rm -rf build  # Clean previous failed build
meson setup build
meson compile -C build
```

**Problem**: `No module named pytest` when using sudo
**Solution**: Use the virtual environment python with sudo:
```bash
# Wrong: sudo python3 -m pytest
# Correct: 
sudo ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml
```

**Problem**: DSA SSH key errors: `ValueError: q must be exactly 160, 224, or 256 bits long`
**Solution**: Generate new RSA SSH keys and configure SSH access:
```bash
# Generate RSA keys (as your regular user, not root)
ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa

# Set up SSH access for root@localhost
ssh-copy-id -i ~/.ssh/id_rsa.pub root@localhost

# Update topology_config.yaml to use your user's key path:
# key_path: /home/your-username/.ssh/id_rsa  (not /root/.ssh/id_rsa)
```

**Problem**: FFmpeg `No such filter: 'drawtext'` when running gen_frames.sh
**Solution**: Install complete FFmpeg build or skip media generation:
```bash
sudo apt install ffmpeg  # Full installation
# Or skip: some tests may work without generated media
```

#### Media File Access
**Problem**: Media files not found
**Solution**: Verify that test media files are available and accessible at the path specified in `media_path`

#### Test Timeouts
**Problem**: Tests timing out on slower systems
**Solution**: Increase timeout values in test_config.yaml for slower systems

### Quick Reference Tables

#### Build Issues

| Problem | Solution |
|---------|----------|
| `RxTxApp: command not found` | Build RxTxApp: `cd tests/tools/RxTxApp && meson setup build && meson compile -C build` |
| `MTL library not found` | Install MTL system-wide: `sudo ninja install -C build && sudo ldconfig` |
| `DSA key error: q must be exactly 160, 224, or 256 bits` | Generate RSA keys: `ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa` |

#### Runtime Issues

| Problem | Solution |
|---------|----------|
| `Permission denied` | Use root user: `sudo ./venv/bin/python3 -m pytest` |
| `No module named pytest` | Don't use `sudo python3`, use `sudo ./venv/bin/python3` |
| `Config path errors` | Update placeholder paths in config files |
| `SSH connection failed` | Ensure SSH keys are set up for root@localhost access |
| `No such filter: 'drawtext'` | Install FFmpeg with text filters or skip media generation |

### Debugging Tests

Use pytest's debug features:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -v --pdb tests/single/st20p/test_st20p_rx.py
```

Increase log verbosity:

```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml --log-cli-level=DEBUG tests/single/st20p/test_st20p_rx.py
```

### Verified Test Criteria

This section defines when a test is eligible to receive the Verified flag and how to validate PASSED reporting, starting with Smoke tests.

Verified flag application (all required):
1. Telemetry matches inputs
   - Example: Input is 60 fps; logs and collected metrics report 60 fps (with a documented tolerance).
   - Example: Configuration flags (e.g., rss_enabled) in logs align with test inputs.

2. Test goal is explicitly validated
   - Each test contains a Doxygen-style preamble describing it (f.e objective, steps, expected outcomes, and metrics)
   - Checks enabled whenever feasible

3. CI gating
   - Test result is PASSED in CI using framework-provisioned environment only (no manual setup).
   - CI artifacts include logs/metrics proving telemetry alignment with inputs.
