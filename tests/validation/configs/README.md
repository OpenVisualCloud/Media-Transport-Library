# Test Configuration

This directory contains configuration files for the Media Transport Library validation test suite. These files define the test environment, network topology, and test parameters.

## Configuration Files

### test_config.yaml

This file contains general test environment settings:

```yaml
build: /path/to/mtl/build
mtl_path: /path/to/mtl
media_path: /mnt/media
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
      - pci_device: 8086:1592
        interface_index: 0 # all
    connections:
      - ip_address: 192.168.1.100
        connection_type: SSHConnection
        connection_options:
          port: 22
          username: user
          password: None
          key_path: /path/to/ssh/key
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
