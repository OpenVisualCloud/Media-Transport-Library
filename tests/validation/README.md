# MTL Validation Framework

The Media Transport Library (MTL) Validation Framework provides comprehensive testing capabilities for various aspects of the MTL, including protocol compliance, performance, and integration testing.

> **For detailed documentation, please refer to [the main validation framework documentation](/doc/validation_framework.md)**

## Quick Start Guide

### Prerequisites

- Python 3.9 or higher
- **⚠️ CRITICAL**: Media Transport Library built and installed (see [build instructions](../../doc/build.md))
- Test media files (typically on NFS)
- Network interfaces configured for testing
- Root privileges for network operations
- FFmpeg and GStreamer plugins (for integration tests)

### Setup in 3 Simple Steps

1. **Ensure MTL is built first** (if not done already):
   ```bash
   cd /path/to/Media-Transport-Library
   ./build.sh
   ```
   See [detailed build instructions](../../doc/build.md) if needed.

2. **Create a virtual environment and install dependencies**:
   ```bash
   cd tests/validation
   python3 -m venv venv
   source venv/bin/activate
   pip install -r requirements.txt
   ```

3. **Configure your environment**:
   - Update network interfaces in `configs/topology_config.yaml`
   - Set correct paths in `configs/test_config.yaml` (especially `build` and `mtl_path`)
   - Ensure media files are accessible at `media_path`

4. **Run tests**:
   ```bash
   # Run smoke tests (quick validation)
   python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke
   
   # Run specific test module
   python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml tests/single/st20p/test_st20p_rx.py
   
   # Generate HTML report
   python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke --template=html/index.html --report=report.html
   ```

## Available Tests

The framework includes tests for:

- **Media Flow Tests**: ST2110-20 (video), ST2110-22 (compressed video), ST2110-30 (audio), ST2110-40 (ancillary data)
- **Backend Tests**: DMA, Kernel Socket, XDP
- **Integration Tests**: FFmpeg, GStreamer
- **Performance Tests**: Throughput, latency, and other metrics

Run tests by category using pytest markers:
```bash
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m [marker]
```

Available markers: `smoke`, `nightly`, `performance`, `dma`, `kernel_socket`, `xdp`, etc.

For more detailed information about configuration options, troubleshooting, and extending the framework, please refer to the [complete documentation](/doc/validation_framework.md).
