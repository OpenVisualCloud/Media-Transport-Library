# RDMA UD Guide

## 1. Background

Remote Direct Memory Access (RDMA) allows direct memory access between computer systems without CPU involvement, reducing latency and increasing throughput.
It's essential for high-performance computing and media transport applications requiring low latency and high bandwidth.
RDMA over Converged Ethernet (RoCE) carries RDMA operations over Ethernet networks.
Unreliable Datagram (UD) is a transport protocol suitable for applications that can tolerate data loss or ensure data integrity independently.

The Intel® Media Transport Library (IMTL) supports RDMA UD backend with IRDMA NICs based on RoCEv2.
This feature is similar to the XDP backend, both bypassing the kernel network stack for data transport.
It is currently experimental and not recommended for production use.

## 2. Install RDMA dependencies

Install the necessary packages:

Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y libibverbs-dev librdmacm-dev libibumad-dev libpci-dev rdma-core infiniband-diags ibverbs-utils
```

Arch Linux:

```bash
sudo pacman -Syu --needed rdma-core
```

## 3. Build IMTL with RDMA UD backend

Refer to the [Build Guide](../build.md) for detailed instructions. Ensure that the libibverbs and librdmacm dependencies are recognized:

```bash
# Output from 'meson setup build'
Run-time dependency libibverbs found: YES 1.14.50.0
Run-time dependency librdmacm found: YES 1.3.50.0
```

## 4. Setup

### 4.1 Enable kernel module

Verify that the `irdma` driver is loaded using `lsmod`.

### 4.2 Configure IP and MTU

The interface should have an IP configured. Use `ifconfig` or `ip a` to check. To set a static IP, use the following commands:

```bash
sudo nmcli dev set ens785f0 managed no
sudo ip addr add 192.168.96.101/24 dev ens785f0
```

Check the current and maximum MTU supported on the device with `ibv_devinfo`. The recommended MTU is 2100 (2048). To set the MTU:

```bash
sudo ip link set dev ens785f0 mtu 2100
```

### 4.3 Configure netns for testing on the same machine

Create netns and assign ports to different namespaces:

```bash
sudo ip netns add rdma0
sudo ip netns add rdma1
sudo ip netns list

sudo ip link set ens785f0 netns rdma0
sudo ip link set ens785f1 netns rdma1

sudo ip netns exec rdma1 ip link list

sudo ip netns exec rdma0 ip addr add 192.168.96.101/24 dev ens785f0
sudo ip netns exec rdma1 ip addr add 192.168.96.102/24 dev ens785f1

sudo ip netns exec rdma0 ip link set dev ens785f0 up
sudo ip netns exec rdma1 ip link set dev ens785f1 up

sudo ip netns exec rdma0 ifconfig ens785f0 mtu 2100
sudo ip netns exec rdma1 ifconfig ens785f1 mtu 2100
```

Run applications with different namespaces using `sudo ip netns exec <ns>` before your commands.

## 5. Run

### 5.1 Running RxTxApp

Configure the network interface in the JSON file:

```json
{
    "interfaces": [
        {
            "name": "rdma_ud:ens785f0",
        }
    ]
}
```

Refer to the [Run Guide](run.md) for more usage details.

RDMA UD supports unicast IP only.

### 5.2 API Usage

For configuring the network interface in your application, use the following code snippet:

```c
/* struct mtl_init_params *p */
snprintf(p->port[i], sizeof(p->port[i]), "%s", "rdma_ud:ens785f0");
p->pmd[i] = mtl_pmd_by_port_name(p->port[i]);
...
mtl_init(p);
```