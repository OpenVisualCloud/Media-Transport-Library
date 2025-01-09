# Kernel Socket Based Guide

The most efficient way to operate this library is based on the high-performance DPDK Poll Mode Driver (PMD), which bypasses the kernel. However, not all Network Interface Cards (NICs) are supported by DPDK. To address this, the library provides a fallback path for non-DPDK NICs, making usage straightforward due to the comprehensive ecosystem of the kernel.
However, it's important to note that performance may be limited compared to DPDK PMD, as the entire data path will go through the kernel stack.

## 1. Prerequisites

Ensure that you have a Network Interface Card (NIC) connected to the network, with an assigned IP address. You can verify this information by using the `ifconfig` command in the terminal.

## 2. Setup Hugepage

For the kernel socket backend, the library still requires the use of huge pages to ensure efficient memory management.

Note: After rebooting the system, it is essential to configure hugepages again, as the configuration will be lost.

For example, if you want to enable 2048 2M huge pages, which would total to 4GB of memory, you can follow the step below:

```bash
sudo sysctl -w vm.nr_hugepages=2048
```

## 3. Run

Config the `interfaces` item in the JSON file to use the kernel network interface, below is an example which use `enp24s0f0` interface.

```json
    "interfaces": [
        {
            "name": "kernel:enp24s0f0",
        },
    ],
```

Also, for complete JSON configurations for ST2110-20 transmission (tx) and reception (rx), please refer to the [kernel tx config](../tests/script/kernel_socket_json/tx.json) and [kernel rx config](../tests/script/kernel_socket_json/rx.json) respectively.

Refer to sections "5. Run the sample application" in the [Linux run guide](run.md) for the detail commands on how to run the sample application.

If you want to select kernel socket data path from the API level, please follow below example:

```c
  struct mtl_init_params* p;
  ...
  p->pmd[MTL_PORT_P] = MTL_PMD_KERNEL_SOCKET;
  snprintf(p->port[MTL_PORT_P], sizeof(p->port[MTL_PORT_P]), "%s", "kernel:enp24s0f0");
  ...
```
