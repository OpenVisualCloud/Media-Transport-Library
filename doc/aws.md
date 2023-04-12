# Running on AWS

## 1. Create AWS instances

Instance type tested: **m6i.4xlarge** (6Gb bandwidth limit)

Image tested: **Amazon Linux 2 AMI**

Created 2 instances (TX and RX) with required storage.

Instance example:

![instance](png/instance.png)

## 2. Install MTL and other software

### 2.1 Build and install DPDK & MTL

Refer to CentOS part of [build.md](./build.md).

### 2.2 Apply vfio-pci patches

Since the default vfio driver does not support WC, ENA has some patches for the kernel.

```shell
git clone https://github.com/amzn/amzn-drivers.git
cd amzn-drivers/userspace/dpdk/enav2-vfio-patch
sudo get-vfio-with-wc.sh
```

## 3. IOMMU Setting

If you use bare metal, you can turn on IOMMU refer to [run.md](./run.md).

If you use VM, set NO-IOMMU mode for vfio after each boot.

```shell
# under root user
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

## 4. Attach interfaces for DPDK

> If you attach extra interfaces before starting the instance, you may not get the public DNS for ssh. The best practice is to **attach after** / **detach before** start.

### 4.1 Create interfaces

Go to  EC2 > Network interfaces > Create network interface.

Choose same subnet for all new interfaces, set the right security groups for your RTP/UDP streams.

### 4.2 Attach interfaces

Right-click on your running instance, go to Networking > Attach network interface, choose an idle interface.

After attaching the interface, remember the Private IPv4 address allocated by AWS, this will be used by kahawai as interface IP.

### 4.3 Bind interface to DPDK PMD

Load vfio-pci module, enable no-iommu mode if IOMMU is not supported.

```shell
# under root user
modprobe vfio-pci
# echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

Unbind the interface from kernel driver and bind to PMD.

```shell
# under root user
ifconfig eth1 down
dpdk-devbind.py -b vfio-pci 0000:00:06.0
# check the interfaces
dpdk-devbind.py -s
```

## 5. Run the application

Refer to [run.md](./run.md) after section 3.2.

The Tx (transmitter) RxTxAPP should add args `--runtime_session` and `--multi_src_port`, see 7.2 and 7.3.

### 5.1 IP configuration

Configure the AWS reserved private IP in json.

For example, the Private IPv4 address is 172.31.42.123, the subnet IPv4 CIDR is 172.31.32.0/20, you can edit the interfaces in json:

```json
    "interfaces": [
        {
            "name": "0000:00:06.0",
            "ip": "172.31.42.123",
            "netmask": "255.255.240.0"
        }
    ],
```

Or you can use DHCP to automatically configure the IPs:

```json
    "interfaces": [
        {
            "name": "0000:00:06.0",
            "proto": "dhcp"
        }
    ],
```

### 5.2 Features not supported on ENA

* **PTP** (use CLOCK_REAL_TIME which may be synced to NTP)
* **Rate Limiting** (use TSC for pacing)
* **rte_flow** (use RSS queues)

## 6. General FAQ

**Q:** Compiler cannot find some dependencies.

**A:** run below commands or add to `/etc/profile`

```shell
export PATH=$PATH:/usr/local/bin/
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/
export LD_LIBRARY_PATH=/usr/local/lib64/
```

---

## 7. Known issues

### 7.1 No ptype support

```shell
MT: Warn: dev_config_port(0), failed to setup all ptype, only 0 supported
```

This is ENA PMD limitation, can be ignored for now.

### 7.2 Setting RSS hash fields is not supported

```shell
ena_rss_hash_set(): Setting RSS hash fields is not supported. Using default values: 0xc30
```

The ENA HW does not support RSS hash fields modification, the app will require same src port and dst port for the stream.

To workaround this limitation, the library uses shared rss mode on ENA by default, arg `--runtime_session` is needed for Tx app.

### 7.3 The max video stream supported is 4k 30fps / 1080p 120fps

The bandwidth for single TX flow (udp/tcp ip:port->ip:port 5 tuple) is limited to 5 / 10 (same placement group) Gbps.

To workaround this limitation, the library uses multiple flows for single stream, arg `--multi_src_port` is needed for Tx app.

## Reference link

[ENA driver repository](https://github.com/amzn/amzn-drivers/tree/master/userspace/dpdk)

[ENA PMD doc](https://doc.dpdk.org/guides/nics/ena.html)

[AWS blog (cn)](https://www.infoq.cn/article/EcQFplTWfdrvumULjo9t)
