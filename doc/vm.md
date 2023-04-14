# VM Setup

We use kvm as hypervisor, as the newer qemu-kvm and kernel support ptp_kvm feature. System real time of VM is synced to host real time with chronyd, the time error between VM and host is about 20ns after synchronization. On host, system real time is adjusted to NIC PF's PHC time by phy2sys/chronyd, and the PHC time is set by ptp4l to get a correct time from PTP grandmaster.

## Host machine setup

(CentOS 8 recommended)

### Enable VT-d, VT-x in BIOS

### Enable IOMMU

```shell
sudo grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"
sudo reboot
```

### Install qemu-kvm

```shell
sudo yum groupinstall "Virtualization Host"
sudo yum install virt-manager
```

### Install latest(>=1.9.11) ICE driver with patches

please refer to the driver update section in [vf guide](vf.md).

### Create VFs (root)

```shell
echo <num> > /sys/class/net/<interface>/device/sriov_numvfs
```  

### Create a VM

1. open virt-manager with GUI
2. add a vm, choose ubuntu 20.04 minimal iso
3. use bridged network or NAT for default NIC
4. specify cpu core, memory, recommend 8 cpus and 8G memory (experimental)
5. configure before install, add a pci passthrough device, choose the created vf
6. start to install vm as normal ubuntu  

### PTP setup

1. install linuxptp: `sudo yum install linuxptp`
2. configure ptp4l daemon  
a. you can use any PF port with PHC support to sync time  
b. `ethtool -T ens801f2` check "PTP Hardware Clock: " is not 0  
c. (optional) manully run ptp4l  

    ```shell
    sudo ptp4l -i ens801f2 -m -s -H
    ```

3. configure phc2sys daemon  
a. (optional) manually run phc2sys  

    ```shell
    sudo phc2sys -s ens801f2 -m -w
    ```  

### Enable IOMMU for VM

```shell
virsh --connect qemu:///system
edit vm0
```  

1. add iommu device to devices

    ```xml
    <devices>
        <iommu model='intel'>
            <driver intremap='on' caching_mode='on' iotlb='on'/>
        </iommu>
        ...
    </devices>
    ```

2. add this to features

    ```xml
    <features>
        <ioapic driver='qemu'/>
        ...
    </features>
    ```

## VM setup

(Ubuntu 20.04 )

### Setup build env, refer to build.md

### PTP setup for VM

1. enable ptp-kvm kernel module, reboot vm

    ```shell
    echo ptp_kvm > /etc/modules-load.d/ptp_kvm.conf
    ```

2. change chronyd config to use PHC0:
edit /etc/chrony/chrony.conf, delete "pool …"<br>
add a line "refclock PHC /dev/ptp0 poll 2"

3. restart chronyd  

    ```shell
    systemctl restart chronyd
    ```

4. check the time sync status, error should be tens of nanoseconds when ready

    ```shell
    $ chronyc sources

    210 Number of sources = 1
    MS Name/IP address         Stratum Poll Reach LastRx Last sample
    ===============================================================================
    #* PHC0                          0   2   377     5     -1ns[   -2ns] +/-   27ns
    ```

### Run Intel® Media Transport Library with `--utc_offset -37`, refer to run.md

## Reference link

[redhat - ptp_kvm](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/virtualization_deployment_and_administration_guide/chap-kvm_guest_timing_management)  
[linuxptp](https://github.com/richardcochran/linuxptp)  
[tsn - ptp4l,phc2sys](https://tsn.readthedocs.io/timesync.html)  
[libvirt - IOMMU](https://libvirt.org/formatdomain.html#iommu-devices)  
