# VM guide for Windows: MTL with E810 or E830 VFs

Best practices for MTL in a Windows VM on a Linux KVM host. Tested on
Proxmox VE 9.2 with Windows 11 Pro guests and E810-C and E830-CC VFs: 1080p59
VF to VF with rate limit pacing, with no frame loss, and 10 st20p streams of
2160p29.97 with a format conversion (52 Gb/s TX and 52 Gb/s RX) on two E810
VFs, with each frame received. VMware ESXi and Hyper-V are not tested.

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

1. **Give each VF a 512-entry RSS LUT** while no VM uses it:
   `echo 512 > /sys/bus/pci/devices/<vf>/rss_lut_vf_attr` (out-of-tree `ice`
   only; `script/nicctl.sh create_vf` does it). The VF then has more than 16
   queue pairs. MTL gives each stream its own queue, so with 16 queues the
   MTL test takes 2 sets; an E830 VF with the large LUT took 10 sets with no
   lost frame. Do not write the LUT while the VF resets (just after a bind,
   or while a VM starts or stops): the write races the reset.

1. **A VF to VF stream needs no cable**: the switch of the adapter carries it.
   A multicast stream also goes out of the PF port, and the network switch
   can send it to each of its ports. If two senders use the same multicast
   group on one network, each receiver gets both streams. Give each sender
   its own group, or send unicast to the MAC of the RX VF
   (`--p_tx_dst_mac <MAC>`, or `run_test.ps1 -DstMac <MAC>`): a unicast
   stream between two VFs of one PF stays in the adapter.

1. **Keep the host off the CPUs of the VM.** Add these kernel arguments for
   the host CPUs of the vCPUs and their SMT siblings, then restart the host
   (here the VM has host CPUs 64-95 of NUMA node 2, and their siblings are
   192-223):

    ```text
    nohz_full=64-95,216-223 rcu_nocbs=64-95,216-223
    isolcpus=managed_irq,domain,64-95,216-223 irqaffinity=0-63,96-215,224-255
    ```

    Also set `kernel.numa_balancing = 0` in a file of `/etc/sysctl.d/`: the
    scan of NUMA balancing stops the vCPU threads. The siblings 192-215 stay
    for the QEMU threads (next step), and 216-223, the siblings of the
    scheduler vCPUs, stay free. Then the host tick stops on these CPUs.
    Measured with one busy thread on each scheduler CPU of the VM: before,
    about 1550 stalls/s of 2.6 us or more and up to 111 stalls of 50 us or
    more in 25 s (the longest 0.9 ms); after, about 550 stalls/s and 0 to 5
    stalls of 50 us or more.

1. **Pin each vCPU to its own host CPU.** `affinity` of Proxmox VE gives the
   QEMU process a list of CPUs: the vCPU threads move in it, and they share it
   with the QEMU threads. A hookscript pins them at each start of the VM. In
   `/var/lib/vz/snippets/pin-vcpus.sh` (the storage `local` must hold the
   content type `snippets`):

    ```bash
    #!/bin/bash
    # vCPU i to host CPU FIRST+i, each other QEMU thread to EMU.
    FIRST=64 EMU=192-215
    [ "$2" = post-start ] || exit 0
    pid=$(cat "/var/run/qemu-server/$1.pid")
    sleep 5
    for c in /proc/"$pid"/task/*/comm; do
        t=${c%/comm}; t=${t##*/}
        if [[ $(cat "$c") =~ ^CPU\ ([0-9]+)/KVM$ ]]; then
            taskset -pc $((FIRST + BASH_REMATCH[1])) "$t"
        else
            taskset -pc "$EMU" "$t"
        fi
    done >/dev/null
    ```

    `chmod 755` the file, then `qm set <vmid> --hookscript
    local:snippets/pin-vcpus.sh --affinity 64-95,192-215`. With libvirt, use
    `<vcpupin>` and `<emulatorpin>` of `<cputune>`.

## VM

| Setting | Value | Why |
| --- | --- | --- |
| Machine | q35, OVMF (UEFI) | PCIe passthrough of the VFs |
| VFs | PCIe passthrough (`hostpci0: <vf>,pcie=1`) | DPDK drives the VF |
| CPU type | `host`, with no `hv_stimer` and no `hv_synic`, and `-overcommit cpu-pm=on` (below) | AVX2 and AVX-512; fewer stalls of the scheduler vCPUs |
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

For a Windows `ostype`, Proxmox VE gives the guest the Hyper-V synthetic timer
(`hv_stimer`, `hv_synic`). Without them, and with `cpu-pm=on` (the halt of an
idle vCPU stays in the guest), the scheduler vCPUs stalled less: a 10 min
test gave 769 in place of 1266 `recalculate warmup` lines. A second `-cpu`
in `args` replaces the one of Proxmox VE (`qm showcmd <vmid>` shows it):

```bash
qm set <vmid> --args "-cpu host,-cet-ibt,-cet-ss,hv_ipi,hv_relaxed,hv_reset,hv_runtime,hv_spinlocks=0x1fff,hv_time,hv_vapic,hv_vpindex,+kvm_pv_eoi,+kvm_pv_unhalt -overcommit cpu-pm=on"
```

With `cpu-pm=on` an idle vCPU takes its host CPU at 100 %, so give the VM
host CPUs that nothing else uses.

## Measured

10 st20p TX and 10 st20p RX sessions, 2160p29.97 with a format conversion,
52 Gb/s TX and 52 Gb/s RX, on two E810 VFs, 10 min each, with the host steps
above. Each frame arrived in each test.

| VM CPU line | MTL processes | `recalculate warmup` per 10 min | Stalls of 0.4-48 ms |
| --- | --- | --- | --- |
| Proxmox VE default | 1 (2 TX schedulers) | 1266 | 3 |
| No `hv_stimer`/`hv_synic`, `cpu-pm=on` | 1 (2 TX schedulers) | 769 | 1 |
| No `hv_stimer`/`hv_synic`, `cpu-pm=on` | 2: TX (1 scheduler) and RX | 429 | none |

The [run guide](run_WIN.md#3-performance) tells how to run TX and RX in two
processes. The cause of the lines that are left is not known yet. Windows
takes about 530 clock interrupts each second on each CPU, 5 us or more each,
with a timer resolution of 15.6 ms.

## Template

Make a template when the build, the drivers and `windows_setup.ps1` are done.
A new VM from it needs only its own VFs, CPU pinning and NUMA node, and a new
computer name (`Rename-Computer`). NetUIO takes the new VFs by itself.

## Limits

- The drivers are test-signed, so this setup is for a lab.
- After a start of Windows, wait about 4 minutes before a test: the start-up
  work of Windows stopped all the vCPUs at one time for up to 4 ms.
- A VM adds the noise of its host. Measure on bare metal for a product.
