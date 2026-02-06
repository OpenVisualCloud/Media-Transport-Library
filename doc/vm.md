# Linux VM Setup Guide

We use kvm as hypervisor, as the newer qemu-kvm and kernel support ptp_kvm feature. System real time of VM is synced to host real time with chronyd, the time error between VM and host is about 20ns after synchronization. On host, system real time is adjusted to NIC PF's PHC time by phy2sys/chronyd, and the PHC time is set by ptp4l to get a correct time from PTP grandmaster.

## 1. Host machine setup

(CentOS 8 recommended)

### 1.1. Enable VT-d, VT-x in BIOS

### 1.2. Enable IOMMU

```bash
sudo grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"
sudo reboot
```

### 1.3. Install qemu-kvm

```bash
sudo yum groupinstall "Virtualization Host"
sudo yum install virt-manager
```

### 1.4. Install latest ICE driver with patches

Please refer to the driver update section in [Intel® E800 Series Ethernet Adapters driver guide](e800_series_drivers.md).

### 1.5. Create VFs

You can also refer to [Run Guide](run.md).

```bash
# root
echo <num> > /sys/class/net/<interface>/device/sriov_numvfs
```

### 1.6. Create a VM

#### 1.6.1. Manually create

* Open virt-manager with GUI
* Add a vm, choose Ubuntu 20.04 minimal iso
* Use bridged network or NAT for default NIC
* Specify cpu core, memory, recommend 8 cpus and 8G memory (experimental)
* Configure before install, add a pci passthrough device, choose the created vf
* Start to install vm as normal Ubuntu

#### 1.6.2. Or create using virt-install

```bash
sudo virt-install \
        --name vm0 \
        --vcpus 8 \
        --cpu host-passthrough \
        --ram 8192 \
        --memballoon none \
        --clock offset='localtime' \
        --network default \
        --graphics vnc,listen=0.0.0.0,port=5901 \
        --video=qxl \
        --disk /path_to_img/vm0.qcow2,size=20,bus=sata \
        --cdrom /path_to_img/ubuntu-20.04.iso \
        --boot cdrom,hd \
        --hostdev pci_0000_af_01_4 \
        --hostdev pci_0000_af_01_5
```

The VFs created are passed into VM by specifying `--hostdev pci_0000_xx_xx_x`.

After running `virt-install` command, the viewer will pop up and you can normally install Ubuntu in the GUI.

### 1.7. PTP setup

* Install linuxptp: `sudo yum install linuxptp`
* Configure ptp4l daemon
    a. you can use any PF port with PHC support to sync time

    b. `ethtool -T ens801f2` check "PTP Hardware Clock: x" is the phc device in /dev/ptpx

    c. (optional) manully run ptp4l

    ```bash
    sudo ptp4l -i ens801f2 -m -s -H
    ```

* Configure phc2sys daemon
    a. (optional) manually run phc2sys

    ```bash
    sudo phc2sys -s ens801f2 -m -w
    ```

### 1.8. Enable IOMMU for VM

```bash
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

* Add memtune hard_limit if you passthrough n(>= 2) PFs/VFs to the VM, the size should be "n * memory + 1GB"

    ```xml
    <memory unit='KiB'>8388608</memory>
    <currentMemory unit='KiB'>8388608</currentMemory>
    <memtune>
        <hard_limit unit='KiB'>17825792</hard_limit>
    </memtune>
    ```

## 2. VM setup

(Ubuntu 20.04 )

### 2.1. Setup build env, refer to build.md

### 2.2. PTP setup for VM

* Enable ptp-kvm kernel module, reboot vm

    ```bash
    sudo bash -c 'echo ptp_kvm > /etc/modules-load.d/ptp_kvm.conf'
    ```

* Install Chrony

    ```bash
    sudo apt update
    sudo apt install chrony
    ```

* Change chronyd config to use PHC0:

    edit /etc/chrony/chrony.conf, delete lines with "pool …"

    add a line `refclock PHC /dev/ptp0 poll 2`

* Restart chronyd

    ```bash
    sudo systemctl restart chronyd
    ```

* Check the time sync status, error should be tens of nanoseconds when ready

    ```bash
    chronyc sources
    ```
    ```text
    210 Number of sources = 1
    MS Name/IP address         Stratum Poll Reach LastRx Lastsample
    ==============================================================================
    #* PHC0                          0   2   377     5     -1n[   -2ns] +/-   27ns
    ```

### 2.3. Run RxTxApp with `--utc_offset -37`, refer to run.md

## Reference link

[Red Hat - ptp_kvm](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/virtualization_deployment_and_administration_guide/chap-kvm_guest_timing_management)  
[Linuxptp](https://github.com/richardcochran/linuxptp)  
[TSN - ptp4l,phc2sys](https://tsn.readthedocs.io/timesync.html)  
[Libvirt - IOMMU](https://libvirt.org/formatdomain.html#iommu-devices)  
