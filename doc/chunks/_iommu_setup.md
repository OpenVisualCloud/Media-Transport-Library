
If you have already enabled IOMMU, you can skip this step. To check if IOMMU is enabled, please verify if there are any IOMMU groups listed under the `/sys/kernel/iommu_groups/` directory. If no groups are found, it indicates that IOMMU is not enabled.

```bash
ls -l /sys/kernel/iommu_groups/
```

#### Enable IOMMU(VT-D and VT-X) in BIOS

The steps to enable IOMMU in your BIOS/UEFI may vary depending on the manufacturer and model of your motherboard. Here are general steps that should guide you:

1. Restart your computer. During the boot process, you'll need to press a specific key to enter the BIOS/UEFI setup. This key varies depending on your system's manufacturer. It's often one of the function keys (like F2, F10, F12), the ESC key, or the DEL key.

2. Navigate to the advanced settings. Once you're in the BIOS/UEFI setup menu, look for a section with a name like "Advanced", "Advanced Options", or "Advanced Settings".

3. Look for IOMMU setting. Within the advanced settings, look for an option related to IOMMU. It might be listed under CPU Configuration or Chipset Configuration, depending on your system. For Intel systems, it's typically labeled as "VT-d" (Virtualization Technology for Directed I/O). Once you've located the appropriate option, change the setting to "Enabled".

4. Save your changes and exit. There will typically be an option to "Save & Exit" or "Save Changes and Reset". Select this to save your changes and restart the computer.

#### Enable IOMMU in Kernel

After enabling IOMMU in the BIOS, you need to enable it in your operating system as well.

##### Ubuntu/Debian

Edit `GRUB_CMDLINE_LINUX_DEFAULT` item in `/etc/default/grub` file, append below parameters into `GRUB_CMDLINE_LINUX_DEFAULT` item if it's not there.

```bash
sudo vim /etc/default/grub
```
```text
intel_iommu=on iommu=pt
```

then:

```bash
sudo update-grub
sudo reboot
```

##### CentOS/RHEL9

```bash
sudo grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"
sudo reboot
```

For non-Intel devices, contact the vendor for how to enable IOMMU.

#### Double Check iommu_groups Creation by Kernel After Reboot

```bash
ls -l /sys/kernel/iommu_groups/
```

If no IOMMU groups are found under the `/sys/kernel/iommu_groups/` directory, it is likely that the previous two steps were not completed as expected. You can use the following two commands to identify which part was missed:

```bash
# Check if "intel_iommu=on iommu=pt" is included
cat /proc/cmdline
# Check if CPU flags have vmx feature
lscpu | grep vmx
```

#### Unlock RLIMIT_MEMLOCK for non-root Run

Skip this step for Ubuntu since the default RLIMIT_MEMLOCK is set to unlimited already.

Some operating systems, including CentOS Stream and RHEL 9, have a small limit to RLIMIT_MEMLOCK (amount of pinned pages the process is allowed to have) which will cause DMA remapping to fail during the running. Please edit `/etc/security/limits.conf`, append below two lines at the end of the file, change <USER> to the username currently logged in.

```text
<USER>    hard   memlock           unlimited
<USER>    soft   memlock           unlimited
```

Reboot the system to let the settings take effect.