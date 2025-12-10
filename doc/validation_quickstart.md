# MTL Validation Framework - Quick Start Guide

This quick start guide helps you get the MTL validation framework running with minimal setup. For detailed information, see the [complete validation framework documentation](validation_framework.md).

## Prerequisites

1. **🏗️ MTL Build Complete**: MTL must be built and test tools available  
   👉 **[Follow complete build instructions](validation_framework.md#setup-and-installation)**

2. **📋 Basic Requirements**:
   - Python 3.9+
   - Root user access (MTL validation requires root privileges for network operations)
   - Network interfaces configured per MTL's [run.md](run.md) (VFs created automatically)
   - Test media files (see [media generation](validation_framework.md#gen_framessh) or use NFS-hosted files)
   - FFmpeg and GStreamer plugins (required for integration tests)
   - Compatible SSH keys (RSA recommended, not DSA)

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
key_path: /home/your-username/.ssh/id_rsa  # YOUR user's SSH key path (not /root/)
ip_address: 127.0.0.1  # For localhost testing
port: 22  # Standard SSH port
```

> **⚠️ SSH Key Setup**:
> - Use your regular user's SSH keys (e.g., `/home/gta/.ssh/id_rsa`), not root's keys
> - If you get DSA key errors, generate new RSA keys:
> ```bash
> ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa
> ssh-copy-id -i ~/.ssh/id_rsa.pub root@localhost
> ```

**[tests/validation/configs/test_config.yaml](../tests/validation/configs/test_config.yaml)**:
```yaml
# Replace MTL_PATH_PLACEHOLDER with your actual paths:
build: /home/gta/Media-Transport-Library/
mtl_path: /home/gta/Media-Transport-Library/
```

### 3. Run Tests
**Basic smoke test** (must run as root):
```bash
cd tests/validation
# Use full path to venv python with sudo:
sudo ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml -m smoke -v
```

> **💡 Root Execution**: Don't use `sudo python3` (uses system python). Use `sudo ./venv/bin/python3` to use the virtual environment.

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

| Common Error | Quick Solution |
|--------------|----------------|
| `RxTxApp: command not found` | [Follow build instructions](validation_framework.md#rxtxapp-test-tool) |
| `Permission denied` | Use root: `sudo ./venv/bin/python3 -m pytest` |
| `No module named pytest` | Don't use `sudo python3`, use `sudo ./venv/bin/python3` |
| `Config path errors` | Update placeholder paths in config files |
| SSH/FFmpeg issues | See [detailed troubleshooting](validation_framework.md#troubleshooting) |

## Generate Test Media (Optional)

For video testing, you may need test media files:  
👉 **[See media generation instructions](validation_framework.md#gen_framessh)**

---

## Documentation Navigation

📖 **Complete Documentation**: [Validation Framework](validation_framework.md) - Detailed information, configuration, and advanced features  
🔧 **Build Issues**: [Build Guide](build.md) - MTL build instructions  
⚙️ **Configuration Help**: [Configuration Guide](configuration_guide.md) - Network and environment setup  

## Summary

This quick start guide gets you running tests in minutes. For production use, detailed configuration, or troubleshooting complex issues, refer to the complete documentation above.