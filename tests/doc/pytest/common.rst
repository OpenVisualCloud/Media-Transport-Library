================
Common Modules
================

Shared utilities and fixtures.

Network Interface Control
=========================

.. automodule:: common.nicctl
   :members:
   :undoc-members:
   :show-inheritance:

Test Fixtures
=============

Defined in conftest.py:

* **hosts**: Host objects from topology
* **build**: MTL build directory path
* **media**: Media files directory path
* **setup_interfaces**: Network interface setup
* **test_time**: Test duration (seconds)
* **test_config**: Configuration from test_config.yaml
* **prepare_ramdisk**: Ramdisk setup (optional)
