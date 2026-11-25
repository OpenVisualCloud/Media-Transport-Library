# Windows VM Setup Guide

## 1. Host machine setup

We use the same CentOS 8 host setup as [vm guide](vm.md). Notice that PTP_KVM is not woking for Windows VM, MTL uses VM system time by default.

### 1.1. Create Windows VM

```bash
# install if not
sudo yum install virt-install virt-viewer

# install virtio-win
sudo yum install -y virtio-win

sudo virt-install \
        --name win_vm0 \
        --vcpus 8 \
        --cpu host-passthrough \
        --ram 8192 \
        --memballoon none \
        --clock offset='localtime' \
        --network default \
        --disk /path_to_img/vm0.qcow2,size=20,bus=sata \
        --cdrom /path_to_img/windows_server_2022.iso \
        --boot cdrom,hd \
        --hostdev pci_0000_af_01_0 \
        --hostdev pci_0000_af_01_1 \
        --graphics vnc,listen=0.0.0.0,port=5901 \
        --video=qxl
```

The VFs created are passed into VM by specifying `--hostdev pci_0000_xx_xx_x`.

After running `virt-install` command, the viewer will pop up and you can normally install Windows in the GUI.

### 1.2. Setup remote desktop access

In virt-viewer window, logon to Windows, go to Start->Settings->System->Remote Desktop, set `Enable Remote Desktop` to `On`.

If you set `--network default` above, the VM should use NAT IP address for the network. You can map the VM RDP port to host.

```bash
# get the assigned IP
sudo virsh net-dhcp-leases default
# enable public access
sudo iptables -I FORWARD -m state -d 192.168.122.0/24 --state NEW,RELATED,ESTABLISHED -j ACCEPT
# RDP(3389) port forward
sudo iptables -t nat -I PREROUTING -p tcp --dport <public_port> -j DNAT --to $VM_IP:3389
```

On you local Windows PC, use `Remote Desktop Connection` or `Remote Desktop (Store)` to connect to [Host_IP]:<public_port>.

## 2. VM setup

### 2.1. Install virtio-win inside VM

Attach the iso drive to VM:

```bash
sudo virt-xml win_vm0 --add-device --disk /usr/share/virtio-win/virtio-win.iso,device=cdrom
```

In Windows VM, go to File Explorer->This PC->virtio-win, Click virtio-win-gt-x64.msi to install.

After the installation, you can detach the virtio iso and Windows iso from cdrom in virt-manager.

### 2.2. Build and run MTL

See [Windows build guide](build_WIN.md) and [Windows run guide](run_WIN.md).

When installing netuio driver, use below command for iavf:

```bash
devcon.exe update netuio.inf "PCI\VEN_8086&DEV_1889"
```

## Reference link

You can find more detailed settings here:

[Installing and managing Windows virtual machines](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/configuring_and_managing_virtualization/installing-and-managing-windows-virtual-machines-on-rhel_configuring-and-managing-virtualization)
