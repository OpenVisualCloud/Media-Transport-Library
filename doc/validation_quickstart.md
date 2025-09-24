# MTL Validation Framework - Quick Start Guide

This quick start guide helps you get the MTL validation framework running with minimal setup. For detailed information, see the [complete validation framework documentation](validation_framework.md).

## Prerequisites (Must Complete First!)

1. **🏗️ Build MTL** (CRITICAL - tests will fail without this):
   ```bash
   cd /path/to/Media-Transport-Library
   ./build.sh
   ```
   > If this fails, see [detailed build instructions](build.md)

2. **📋 Basic Requirements**:
   - Python 3.9+
   - Root user access (MTL validation requires root privileges)
   - Network interfaces configured for testing

## Quick Setup (3 steps)

### 1. Install Dependencies
**Run in tests/validation directory**:
```bash
cd tests/validation
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt  # Main framework requirements
pip install -r common/integrity/requirements.txt  # Integrity test components
```

### 2. Configure Environment
Update two key files:

**[tests/validation/configs/topology_config.yaml](../tests/validation/configs/topology_config.yaml)**:
```yaml
# Key settings to update:
username: root  # Must be root for MTL operations
key_path: /root/.ssh/id_rsa  # Your SSH key path
```

**[tests/validation/configs/test_config.yaml](../tests/validation/configs/test_config.yaml)**:
```yaml
# Replace MTL_PATH_PLACEHOLDER with your actual paths:
build: /home/gta/Media-Transport-Library/
mtl_path: /home/gta/Media-Transport-Library/
```

### 3. Run Tests
**Basic smoke test**:
```bash
cd tests/validation
source venv/bin/activate
python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke -v
```

**Run specific test with parameters**:
```bash
pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]"
```

## Optional: Create VFs for Advanced Testing

If you need VFs for NIC testing:
```bash
# Find your network device first
lspci | grep Ethernet

# Create VFs (replace with your device identifier)
sudo ./script/nicctl.sh create_vf ${TEST_PF_PORT_P}
sudo ./script/nicctl.sh create_vf ${TEST_PF_PORT_R}
```

## Quick Troubleshooting

| Error | Solution |
|-------|----------|
| `RxTxApp: command not found` | Build MTL first with `./build.sh` |
| `Permission denied` | Use root user (not regular user) |
| `Config path errors` | Update placeholder paths in config files |

## Generate Test Media (Optional)

For video testing, generate test frames:
```bash
cd tests/validation/common
./gen_frames.sh
```

The script supports:
- Multiple resolutions (3840x2160, 1920x1080, 1280x720, 640x360)
- Different pixel formats (yuv422p, yuv422p10le)
- Configurable color patterns and test signals
- Various frame rates

---

**Need more details?** → [Complete Documentation](validation_framework.md)  
**Build issues?** → [Build Guide](build.md)  
**Configuration help?** → [Configuration Guide](configuration_guide.md)