# MTL Validation Framework

The Media Transport Library (MTL) Validation Framework provides comprehensive testing capabilities for various aspects of the MTL, including protocol compliance, performance, and integration testing.

## Documentation Navigation

📖 **Complete Documentation**: [Main validation framework documentation](../../doc/validation_framework.md) - Detailed configuration, troubleshooting, and advanced features  
🚀 **Quick Start**: [Validation Quick Start Guide](../../doc/validation_quickstart.md) - Get running in 3 steps  
🔧 **Build Issues**: [Build Guide](../../doc/build.md) - MTL build instructions  

---

## Quick Setup

### Prerequisites

- Python 3.9 or higher
- **⚠️ CRITICAL**: Media Transport Library built and installed (see [build instructions](../../doc/build.md))
- Test media files (typically on NFS)
- Network interfaces configured for testing
- **Root privileges required** (MTL validation must run as root user)
- FFmpeg and GStreamer plugins (for integration tests)

### Setup in 3 Simple Steps

1. **🏗️ MTL Build**: Ensure MTL and test tools are built  
   👉 **[Complete build instructions](../../doc/validation_framework.md#setup-and-installation)**

2. **⚡ Quick Setup**: Follow 3-step setup process  
   👉 **[Quick Start Guide](../../doc/validation_quickstart.md)**

3. **🏃 Run Tests**: Execute validation tests
   ```bash
   # Quick smoke test
   sudo ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke
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
