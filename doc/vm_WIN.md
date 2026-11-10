# VM guide for Windows: MTL with E810 or E830 VFs

Best practices for MTL in a Windows VM on a Linux KVM host. Tested on
Proxmox VE 9.2 with Windows 11 Pro guests and E810-C and E830-CC VFs: 1080p59
VF to VF with rate limit pacing, with no frame loss. VMware ESXi and Hyper-V
are not tested.

Then build and run MTL in the VM: [build guide](build_WIN.md),
[run guide](run_WIN.md).

## Host

1. **Use the out-of-tree `ice` driver** of `versions.env`
   (`script/build_drivers.sh --driver ice`). The in-tree driver of the kernel
   gives a VF no rate limit and no flow rules. Build it again after each new
   host kernel. If the host has the in-tree `irdma`, block it: it does not
   match the out-of-tree driver.

1. **Make the VFs at boot, before the VMs start** (a systemd unit with
   `Before=pve-guests.service` on Proxmox VE). Two VFs per PF, each with a
   fixed MAC, trust on and spoof check off:

    ```bash
    echo 2 > /sys/bus/pci/devices/<pf>/sriov_numvfs
    ip link set <pf_netdev> vf 0 mac 02:00:00:00:00:01 trust on spoofchk off
    ip link set <pf_netdev> vf 1 mac 02:00:00:00:00:02 trust on spoofchk off
    ```

1. **A VF to VF stream needs no cable**: the switch of the adapter carries it.
   But the stream also goes out of the PF port. If two senders use the same
   multicast group on one network, each receiver gets both streams. Give each
   sender its own group.

## VM

| Setting | Value | Why |
| --- | --- | --- |
| Machine | q35, OVMF (UEFI) | PCIe passthrough of the VFs |
| VFs | PCIe passthrough (`hostpci0: <vf>,pcie=1`) | DPDK drives the VF |
| CPU type | `host` | AVX2 and AVX-512 |
| vCPUs | Pinned to host CPUs of the NUMA node of the adapter, which no other VM and no host service uses | The scheduler threads of MTL poll at 100 %. A busy host CPU makes them late. |
| Memory | Bound to the same NUMA node, ballooning off | Passthrough pins the memory |
| Secure Boot | Off: EFI variables with no enrolled keys | The drivers are test-signed |

Proxmox VE example, with the adapter on NUMA node 2 and host CPUs 64-79 on that
node:

```bash
qm set <vmid> --hostpci0 <vf0>,pcie=1 --hostpci1 <vf1>,pcie=1
qm set <vmid> --cpu host --cores 16 --numa 1 \
    --numa0 cpus=0-15,hostnodes=2,memory=32768,policy=bind --affinity 64-79 --balloon 0
qm set <vmid> --efidisk0 local-lvm:1,efitype=4m,pre-enrolled-keys=0
```

Suspend BitLocker in the VM before you change the EFI disk.

## Template

Make a template when the build, the drivers and `windows_setup.ps1` are done.
A new VM from it needs only its own VFs, CPU pinning and NUMA node, and a new
computer name (`Rename-Computer`). NetUIO takes the new VFs by itself.

## Limits

- The drivers are test-signed, so this setup is for a lab.
- One MTL process for each VM.
- A VM adds the noise of its host. Measure on bare metal for a product.
