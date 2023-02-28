# Running on AWS

## 1. Create AWS instances

Instance type tested: **m6i.4xlarge** (6Gb bandwidth limit)

Image tested: **Amazon Linux 2 AMI**

Created 2 instances (TX and RX) with required storage.

Instance example:

![instance](png/instance.png)

## 2. Hugepage setting

Currently with no-iommu mode you have to enable 1G hugepage.

```shell
# under root user
echo GRUB_CMDLINE_LINUX="default_hugepagesz=1G hugepagesz=1G hugepages=4">>/etc/default/grub
grub2-mkconfig -o /boot/grub2/grub.cfg
```

Reboot instance.

## 3. Install MTL and other software

### 3.1 Build and install DPDK & MTL

Refer to CentOS part of [build.md](./build.md).

### 3.2 Apply vfio-pci patches

Since the default vfio driver does not support WC, ENA has some patches for the kernel.

```shell
git clone https://github.com/amzn/amzn-drivers.git
cd amzn-drivers/userspace/dpdk/enav2-vfio-patch
sudo get-vfio-with-wc.sh
```

## 4. IOMMU Setting

If you use bare metal, you can turn on IOMMU refer to [run.md](./run.md).

If you use VM, set NO-IOMMU mode for vfio after each boot.

```shell
# under root user
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

## 5. Attach interfaces for DPDK

> If you attach extra interfaces before starting the instance, you may not get the public DNS for ssh. The best practice is to **attach after** / **detach before** start.

### 5.1 Create interfaces

Go to  EC2 > Network interfaces > Create network interface.

Choose same subnet for all new interfaces, set the right security groups for your RTP/UDP streams.

### 5.2 Attach interfaces

Right-click on your running instance, go to Networking > Attach network interface, choose an idle interface.

After attaching the interface, remember the Private IPv4 address allocated by AWS, this will be used by kahawai as interface IP.

### 5.3 Bind interface to DPDK PMD

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

## 6. Run the application

Refer to [run.md](./run.md) after section 3.3.

### 6.1 IP configuration

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

### 6.2 Features not supported on ENA

* **PTP** (use system real_time)
* **Rate Limiting** (use TSC for pacing)
* **rte_flow** (use RSS queues)

## 7. General FAQ

**Q:** Compiler cannot find some dependencies.

**A:** run below commands or add to `/etc/profile`

```shell
export PATH=$PATH:/usr/local/bin/
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig/
export LD_LIBRARY_PATH=/usr/local/lib64/
```

---

**Q:** Cannot find ninja.

**A:** Edit `build.sh`, remove `sudo` in it, then run `build.sh` under root user.

---

## 8. Known issues

### 8.1 2M hugepages cannot be used under no_iommu mode or with igb_uio driver

As one 1080p frame needs 5MB memory, when buffer allocated, the physical memory may locate on different but not continious pages, this will cause invalid payload sent from TX.

You have to enable 1G hugepages.

### 8.2 No ptype support

```shell
MT: Warn: dev_config_port(0), failed to setup all ptype, only 0 supported
```

This is ENA PMD limitation, can be ignored for now.

### 8.3 Setting RSS hash fields is not supported

```shell
ena_rss_hash_set(): Setting RSS hash fields is not supported. Using default values: 0xc30
```

This is ENA NIC hardware limitation, can be ignored for now.

## Reference link

[ENA driver repository](https://github.com/amzn/amzn-drivers/tree/master/userspace/dpdk)

[ENA PMD doc](https://doc.dpdk.org/guides/nics/ena.html)

[AWS blog (cn)](https://www.infoq.cn/article/EcQFplTWfdrvumULjo9t)
