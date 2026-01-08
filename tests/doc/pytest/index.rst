==================
Python Tests
==================

End-to-end validation of MTL functionality using pytest and RxTxApp.

.. toctree::
   :maxdepth: 3

   kernel_socket
   common

Test Framework
==============

Built on pytest with custom plugins:

* **mfd-config**: Topology and test parameter management
* **mfd-connect**: Remote host connections
* **mfd-logging**: Enhanced test reporting
* **mfd-network-adapter**: Network interface management

How Tests Work
==============

1. Setup network interfaces (VF creation, driver binding)
2. Generate RxTxApp JSON configuration
3. Execute TX and RX processes
4. Validate output (frames, rates, integrity)
5. Cleanup resources

Configuration
=============

topology_config.yaml
--------------------

Test environment::

    hosts:
      - name: localhost
        network_interfaces:
          - pci_address: 0000:31:00.0
          - pci_address: 0000:31:00.1

test_config.yaml
----------------

Test parameters::

    build: /path/to/build
    mtl_path: /path/to/mtl
    media_path: /path/to/media
    test_time: 30
    interface_type: VF
