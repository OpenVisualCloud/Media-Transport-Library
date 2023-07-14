# Linux VM Setup Guide

We use kvm as hypervisor, as the newer qemu-kvm and kernel support ptp_kvm feature. System real time of VM is synced to host real time with chronyd, the time error between VM and host is about 20ns after synchronization. On host, system real time is adjusted to NIC PF's PHC time by phy2sys/chronyd, and the PHC time is set by ptp4l to get a correct time from PTP grandmaster.

## 1. Host machine setup

(CentOS 8 recommended)

### 1.1 Enable VT-d, VT-x in BIOS

### 1.2 Enable IOMMU

```shell
sudo grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"
sudo reboot
```

### 1.3 Install qemu-kvm

```shell
sudo yum groupinstall "Virtualization Host"
sudo yum install virt-manager
```

### 1.4 Install latest ICE driver with patches

please refer to the driver update section in [Intel® E810 Series Ethernet Adapter driver guide](e810.md)

### 1.5 Create VFs

```shell
# root
echo <num> > /sys/class/net/<interface>/device/sriov_numvfs
```  

### 1.6 Create a VM

* Open virt-manager with GUI
* Add a vm, choose ubuntu 20.04 minimal iso
* Use bridged network or NAT for default NIC
* Specify cpu core, memory, recommend 8 cpus and 8G memory (experimental)
* Configure before install, add a pci passthrough device, choose the created vf
* Start to install vm as normal ubuntu

### 1.7 PTP setup

* Install linuxptp: `sudo yum install linuxptp`
* Configure ptp4l daemon
    a. you can use any PF port with PHC support to sync time

    b. `ethtool -T ens801f2` check "PTP Hardware Clock: " is not 0

    c. (optional) manully run ptp4l

    ```shell
    sudo ptp4l -i ens801f2 -m -s -H
    ```

* Configure phc2sys daemon
    a. (optional) manually run phc2sys

    ```shell
    sudo phc2sys -s ens801f2 -m -w
    ```  

### 1.8 Enable IOMMU for VM

```shell
virsh --connect qemu:///system
edit vm0
```  

* Add iommu device to devices

    ```xml
    <devices>
        <iommu model='intel'>
            <driver intremap='on' caching_mode='on' iotlb='on'/>
        </iommu>
        ...
    </devices>
    ```

* Add this to features

    ```xml
    <features>
        <ioapic driver='qemu'/>
        ...
    </features>
    ```

## 2. VM setup

(Ubuntu 20.04 )

### 2.1 Setup build env, refer to build.md

### 2.2 PTP setup for VM

* Enable ptp-kvm kernel module, reboot vm

    ```shell
    echo ptp_kvm > /etc/modules-load.d/ptp_kvm.conf
    ```

* Change chronyd config to use PHC0:

    edit /etc/chrony/chrony.conf, delete "pool …"

    add a line "refclock PHC /dev/ptp0 poll 2"

* Restart chronyd

    ```shell
    systemctl restart chronyd
    ```

* Check the time sync status, error should be tens of nanoseconds when ready

    ```shell
    $ chronyc sources
    210 Number of sources = 1
    MS Name/IP address         Stratum Poll Reach LastRx Lastsample
    ==============================================================================
    #* PHC0                          0   2   377     5     -1n[   -2ns] +/-   27ns
    ```

### 2.3 Run RxTxApp with `--utc_offset -37`, refer to run.md

## Reference link

[Red Hat - ptp_kvm](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/virtualization_deployment_and_administration_guide/chap-kvm_guest_timing_management)  
[Linuxptp](https://github.com/richardcochran/linuxptp)  
[TSN - ptp4l,phc2sys](https://tsn.readthedocs.io/timesync.html)  
[Libvirt - IOMMU](https://libvirt.org/formatdomain.html#iommu-devices)  
