# Running on AWS

## 1. Create AWS instances

Instance type tested: **m6i.nxlarge**, **m6i.metal**

(check the bandwidth limitation here: [network-performance](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/general-purpose-instances.html#general-purpose-network-performance))

Images tested: **Amazon Linux 2023, Amazon Linux 2**

Create 2 instances (TX and RX) with required storage.

Instance example:

![instance](png/instance.png)

## 2. Install Media Transport Library and other software

### 2.1 Build and install DPDK & Media Transport Library

Refer to CentOS part of [build.md](./build.md).

### 2.2 Apply vfio-pci patches

Since the default vfio driver does not support WC(Write Combining), patches should be applied for the kernel.

```bash
git clone https://github.com/amzn/amzn-drivers.git
cd amzn-drivers/userspace/dpdk/enav2-vfio-patch
sudo get-vfio-with-wc.sh
```

## 3. IOMMU Setting

If you use bare metal, you can turn on IOMMU refer to [run.md](./run.md).

If you use VM, set NO-IOMMU mode for vfio after each boot.

```bash
sudo modprobe vfio-pci
sudo bash -c 'echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode'
```

## 4. Attach interfaces for DPDK

> If you attach extra interfaces before starting the instance, you may not get the public DNS for ssh. The best practice is to **attach after** / **detach before** start.

### 4.1 Create interfaces

Go to `EC2 > Network interfaces > Create network interface`.

Choose same subnet for all new interfaces, set the right security groups for your RTP/UDP streams. (Usually allow all traffic from same subnet.)

### 4.2 Attach interfaces

Right-click on your running instance, go to `Networking > Attach network interface`, choose an idle interface.

After attaching the interface, remember the Private IPv4 address allocated by AWS, this will be used by Media Transport Library as interface IP.

### 4.3 Bind interface to DPDK PMD

Unbind the interface from kernel driver and bind to PMD.

```bash
sudo ifconfig eth1 down
sudo dpdk-devbind.py -b vfio-pci 0000:00:06.0
# check the interfaces
dpdk-devbind.py -s
```

## 5. Run the application

> If no IOMMU support(.nxlarge instance), you have to run it under root user.

Refer to [run.md](./run.md) after section 3.2.

For single video stream whose bandwidth is grater than 5 Gbps (4k 30fps), arg `--multi_src_port` is needed in Tx app, see 7.3.

### 5.1 IP configuration

Configure the AWS reserved private IP in json.

You can enable DHCP to automatically configure the IPs:

```json
    "interfaces": [
        {
            "name": "0000:00:06.0",
            "proto": "dhcp"
        }
    ],
```

Or you can manually set the IPs(which should work with current security group). For example, the Private IPv4 address is 172.31.42.123, the subnet IPv4 CIDR is 172.31.32.0/20, you can edit the interfaces in json:

```json
    "interfaces": [
        {
            "name": "0000:00:06.0",
            "ip": "172.31.42.123",
            "netmask": "255.255.240.0"
        }
    ],
```

### 5.2 Features not supported on ENA

| Feature| Solution / work around |
| :--- | :--- |
|**PTP** | use CLOCK_REAL_TIME which can be synced by NTP|
|**Rate Limiting** | use TSC for pacing|
|**rte_flow** | use RSS queues|

## 6. General FAQ

**Q:** Compiler cannot find some dependencies.

**A:** run below commands before starting the app

```bash
export PATH=$PATH:/usr/local/bin/
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/
export LD_LIBRARY_PATH=/usr/local/lib64/
```

---

## 7. Known issues

### 7.1 No IOMMU support for VM(.nxlarge) instance

To use the ENA PMD, IOMMU support is required. However, the .nxlarge instance does not support IOMMU, so the vfio driver must be run in no-IOMMU mode. **Running the app under the root user is necessary.**

### 7.2 No ptype support

```bash
MT: Warn: dev_config_port(0), failed to setup all ptype, only 0 supported
```

This is ENA PMD limitation, can be ignored for now.

### 7.3 Setting RSS hash fields is not supported (WA fixed)

```bash
ena_rss_hash_set(): Setting RSS hash fields is not supported. Using default values: 0xc30
```

The ENA HW does not support RSS hash fields modification, the app will require known src port and dst port for the stream.

To workaround this limitation, the library uses shared rss mode on ENA by default which will receive and handle packets in one thread.

### 7.4 The max single video stream supported is 4k 30fps / 1080p 120fps (WA fixed)

The bandwidth for single flow (udp ip:port->ip:port 5 tuple) is limited to 5 / 10(same placement group) Gbps.

To workaround this limitation, the library uses multiple flows for single stream, arg `--multi_src_port` is needed for Tx app.

## Reference link

[ENA driver repository](https://github.com/amzn/amzn-drivers/tree/master/userspace/dpdk)

[ENA PMD doc](https://doc.dpdk.org/guides/nics/ena.html)

[AWS blog (cn)](https://www.infoq.cn/article/EcQFplTWfdrvumULjo9t)
