# Test Configuration

This directory contains configuration files for the Media Transport Library validation test suite. These files define the test environment, network topology, and test parameters.

## ⚠️ Critical Setup Required

**BEFORE RUNNING TESTS**: You must update the placeholder values in these configuration files with your actual system details. Tests will fail with default placeholder values.

## Configuration Files

### test_config.yaml

This file contains general test environment settings:

```yaml
build: MTL_PATH_PLACEHOLDER                    # ⚠️ UPDATE: Path to your MTL installation
mtl_path: MTL_PATH_PLACEHOLDER                 # ⚠️ UPDATE: Same as build path
media_path: /mnt/media                         # ⚠️ UPDATE: Path to your test media files
capture_cfg:
  enable: false
  test_name: test_name
  pcap_dir: /mnt/ramdisk/pcap
  capture_time: 5
  interface: null
ramdisk:
  media: 
    mountpoint: /mnt/ramdisk/media
    size_gib: 32
  pcap:
    mountpoint: /mnt/ramdisk/pcap
    size_gib: 768
```

#### Key Parameters

- **build**: Path to the Media Transport Library build directory
- **mtl_path**: Path to the Media Transport Library installation  
- **media_path**: Path to the directory containing test media files

#### ⚠️ Setup Instructions

1. **Replace `MTL_PATH_PLACEHOLDER`** with your actual MTL installation path:
   ```bash
   # Example: if MTL is in /home/user/Media-Transport-Library/
   build: /home/user/Media-Transport-Library/
   mtl_path: /home/user/Media-Transport-Library/
   ```

2. **Update `media_path`** to point to your test media files location

3. **Verify the paths exist**:
   ```bash
   ls /path/to/your/Media-Transport-Library/build
   ls /path/to/your/media/files/
   ```

#### Other Parameters
- **capture_cfg**: Network packet capture configuration
  - **enable**: Enable/disable packet capture
  - **test_name**: Name prefix for capture files
  - **pcap_dir**: Directory to store capture files
  - **capture_time**: Duration of packet capture in seconds
  - **interface**: Network interface to capture from
- **ramdisk**: RAM disk configuration for high-performance testing
  - **media.mountpoint**: Mount point for media RAM disk
  - **media.size_gib**: Size of media RAM disk in GiB
  - **pcap.mountpoint**: Mount point for packet capture RAM disk
  - **pcap.size_gib**: Size of packet capture RAM disk in GiB

### topology_config.yaml

This file defines the network topology for testing:

```yaml
---
metadata:
  version: '2.4'
hosts:
  - name: host
    instantiate: true
    role: sut
    network_interfaces:
      - pci_device: 8086:1592              # ⚠️ UPDATE: Your NIC's PCI device ID
        interface_index: 0 # all
    connections:
      - ip_address: IP_ADDRESS_PLACEHOLDER # ⚠️ UPDATE: Your system IP
        connection_type: SSHConnection
        connection_options:
          port: SSH_PORT_PLACEHOLDER       # ⚠️ UPDATE: SSH port (usually 22)
          username: USERNAME_PLACEHOLDER   # ⚠️ UPDATE: Your username
          password: None
          key_path: KEY_PATH_PLACEHOLDER   # ⚠️ UPDATE: Path to your SSH key
```

#### ⚠️ Setup Instructions

1. **Find your PCI device ID**:
   ```bash
   lspci | grep Ethernet
   # Look for output like: 86:00.0 Ethernet controller: Intel Corporation...
   # Use format: 8086:XXXX (8086 = Intel vendor ID)
   ```

2. **Update placeholder values**:
   ```yaml
   # Replace placeholders with actual values:
   ip_address: 127.0.0.1        # For localhost, or your actual IP
   port: 22                     # SSH port
   username: your_actual_user   # Your username
   key_path: /home/your_user/.ssh/id_rsa  # Path to your SSH key
   ```

3. **Verify SSH key exists**:
   ```bash
   ls -la ~/.ssh/id_rsa
   # If missing, generate one: ssh-keygen -t rsa -b 4096
   ```

#### Topology Parameters

- **metadata.version**: Configuration format version
- **hosts**: List of hosts in the test topology
  - **name**: Host identifier
  - **instantiate**: Whether to instantiate the host
  - **role**: Host role (e.g., sut for System Under Test)
  - **network_interfaces**: List of network interfaces
    - **pci_device**: PCI device ID
    - **interface_index**: Interface index
  - **connections**: List of connections to the host
    - **ip_address**: Host IP address
    - **connection_type**: Type of connection
    - **connection_options**: Connection parameters
      - **port**: SSH port
      - **username**: SSH username
      - **password**: SSH password (or None for key-based authentication)
      - **key_path**: Path to SSH private key

## Customizing Configurations

### Environment-Specific Configuration

To customize the configuration for different environments, create copies of these files with environment-specific settings:

1. Copy `test_config.yaml` to `test_config.local.yaml`
2. Modify the parameters as needed
3. The test framework will prioritize `.local.yaml` files over the default ones

### Temporary Configuration Changes

For temporary configuration changes during test development:

1. Modify the parameters directly in the YAML files
2. Run your tests
3. Revert changes when done or use git to discard changes

### Programmatic Configuration Overrides

Test modules can programmatically override configuration values:

```python
def test_with_custom_config(config):
    # Override configuration for this test
    config.capture_cfg.enable = True
    config.capture_cfg.interface = "enp1s0f0"
    
    # Run test with modified configuration
    # ...
```

## License

BSD-3-Clause License
Copyright (c) 2024-2025 Intel Corporation
