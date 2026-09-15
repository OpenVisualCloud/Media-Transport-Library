# CPU Isolation Guide

This guide covers CPU isolation for the Media Transport Library (MTL). Isolating CPU cores from general OS scheduling ensures that latency-sensitive MTL tasklets run with minimal interference from kernel housekeeping, interrupts, and other processes.

MTL uses busy-polling threads (DPDK lcores) pinned to individual CPU cores. Without isolation, kernel activity — scheduler ticks, workqueues, timers, and RCU callbacks — can preempt these threads and introduce jitter, causing packet timing violations in ST2110 streams.

---

## 1. Static CPU Isolation (Kernel Boot Parameters)

Static isolation is configured via kernel boot parameters and requires a **reboot** to take effect.

### 1.1. `isolcpus`

```text
isolcpus=[flag-list,]<cpu-list>
```

Isolates CPUs from general-purpose scheduling and kernel disturbances. The default flag is `domain` if none are specified.

**Flags:**

| Flag | Effect |
|------|--------|
| `domain` | Removes CPUs from SMP load balancing. **Irreversible until reboot.** |
| `nohz` | Disables scheduler ticks on isolated CPUs and offloads RCU callbacks (equivalent to `nohz_full`). Requires `CONFIG_NO_HZ_FULL=y` in the kernel — see [section 1.3](#13-kernel-config-prerequisites). |
| `managed_irq` | Best-effort isolation from managed interrupts whose affinity masks include isolated CPUs. |

**Example** — isolate CPUs 2–15 with full isolation:

```bash
# /etc/default/grub  (append to GRUB_CMDLINE_LINUX)
isolcpus=domain,managed_irq,nohz 2-15
```

Then regenerate GRUB and reboot:

```bash
sudo grub2-mkconfig -o /boot/grub2/grub.cfg   # RHEL / Rocky / CentOS
# or
sudo update-grub                                # Ubuntu / Debian
sudo reboot
```

> **Note:** `isolcpus` is considered deprecated for domain isolation. For dynamic or runtime-adjustable isolation, prefer cpusets (see [section 2](#2-dynamic-cpu-isolation-cpusets)).

### 1.2. `nohz_full`

```text
nohz_full=<cpu-list>
```

Provides the same tick-less behavior as `isolcpus=nohz` but does **not** isolate scheduler domains or managed IRQs. Can be used standalone or in combination with `isolcpus=domain,managed_irq`.

**Example:**

```text
nohz_full=2-15
```

### 1.3. Kernel Config Prerequisites

Full tick suppression (`nohz` / `nohz_full`) requires the kernel to be built with `CONFIG_NO_HZ_FULL=y`. Without it, the `nohz` flag in `isolcpus` is silently ignored and scheduler ticks continue on "isolated" CPUs.

**Verify your kernel:**

```bash
grep -E 'CONFIG_NO_HZ_FULL|CONFIG_RCU_FAST_NO_HZ' /boot/config-$(uname -r)
```

Expected output for a properly configured kernel:

```text
CONFIG_NO_HZ_FULL=y
CONFIG_RCU_FAST_NO_HZ=y
```

> **If `CONFIG_NO_HZ_FULL` is not set:** Remove the `nohz` flag from your `isolcpus` arguments. Using `nohz` on a kernel without this config option can cause the entire `isolcpus` argument to be silently ignored, resulting in **no isolation at all**.

Reference: <https://docs.kernel.org/timers/no_hz.html>

### 1.4. Workqueue CPU Mask

The sysfs file `/sys/devices/virtual/workqueue/cpumask` controls which CPUs run global kernel workqueues. By default it includes **all** CPUs, so kernel workqueue activity can still run on isolated CPUs.

When `isolcpus` or `nohz_full` is set correctly, the kernel should automatically adjust this mask to exclude isolated CPUs.

**Verify:**

```bash
cat /sys/devices/virtual/workqueue/cpumask
```

#### How to Read the `cpumask`

The mask is displayed in hexadecimal, split into 32-bit chunks separated by commas. The **rightmost** chunk represents CPUs 0–31. The least significant bit (rightmost) is CPU 0. A `1` bit means the CPU is allowed to run workqueues; `0` means excluded.

**Example:** Housekeeping CPUs 0–47, isolated CPUs 48–51 on a 52-core system:

```text
000fffff,ffffffff
```

| Chunk | Binary meaning |
|-------|---------------|
| `ffffffff` | CPUs 0–31 are all allowed |
| `000fffff` | CPUs 32–51: bits 32–47 set (allowed), bits 48–51 clear (excluded) |

Another example — if CPUs 48–51 are isolated on a larger system you may see:

```text
ffffffff,ffffffff,ffffffff,ffffffff,fff0ffff,ffffffff
```

If isolated CPUs still appear as `1` in this mask, the kernel did not automatically exclude them. This indicates a configuration problem — revisit the `isolcpus` arguments and kernel config.

### 1.5. Verification with `stress-ng`

Use `stress-ng` to confirm that isolated CPUs are not receiving general workloads.

```bash
sudo dnf install stress-ng    # RHEL / Rocky / CentOS
# or
sudo apt install stress-ng    # Ubuntu / Debian
```

Run a CPU stress test that exceeds the total core count:

```bash
stress-ng --cpu 300 --timeout 60s
```

While the stress test is running, verify:

1. **Isolated CPUs are idle** — use `htop`, `mpstat`, or `top` to confirm that isolated cores show near-zero utilization:

    ```bash
    mpstat -P ALL 1
    ```

2. **Workqueue mask excludes isolated CPUs:**

   ```bash
   cat /sys/devices/virtual/workqueue/cpumask
   ```

3. **Kernel config supports nohz** (if using the `nohz` flag):

   ```bash
   grep -E 'CONFIG_NO_HZ_FULL|CONFIG_RCU_FAST_NO_HZ' /boot/config-$(uname -r)
   ```

If isolated CPUs show significant load during the stress test, check your boot parameters and kernel configuration.

### 1.6. Limitations of Static Isolation

- Changes require a **reboot**.
- `isolcpus=domain` is **not reversible** at runtime.
- `isolcpus` is officially deprecated for domain isolation in newer kernels.
- Prefer cpusets for dynamic, runtime-adjustable isolation (see [section 2](#2-dynamic-cpu-isolation-cpusets)).

---

## 2. Dynamic CPU Isolation (cpusets)

For environments where rebooting is impractical — such as busy production boxes, shared servers, or containerized deployments — **cpusets** provide runtime-adjustable CPU isolation via cgroups v2.

### 2.1. Create an Isolated cpuset

```bash
# Create a dedicated cgroup for MTL
sudo mkdir -p /sys/fs/cgroup/mtl

# Assign CPUs (e.g., CPUs 2-15) and memory node
echo "2-15" | sudo tee /sys/fs/cgroup/mtl/cpuset.cpus
echo "0"    | sudo tee /sys/fs/cgroup/mtl/cpuset.mems

# Disable load balancing on this cpuset so the scheduler
# does not migrate other tasks onto these CPUs
echo "0" | sudo tee /sys/fs/cgroup/mtl/cpuset.sched_load_balance
```

### 2.2. Move the MTL Process into the cpuset

```bash
# Move by PID
echo $MTL_PID | sudo tee /sys/fs/cgroup/mtl/cgroup.procs
```

Or launch directly into the cpuset:

```bash
sudo cgexec -g cpuset:mtl ./tests/tools/RxTxApp/build/RxTxApp --config_file config/tx_1v.json --lcores 2,3,4,5
```

### 2.3. Confine Everything Else to Housekeeping CPUs

To prevent other processes from running on the isolated CPUs, restrict the root cpuset:

```bash
# Move all existing tasks to housekeeping CPUs (0-1, 16+)
echo "0-1,16-$(nproc --all)" | sudo tee /sys/fs/cgroup/cpuset.cpus
```

Or use `taskset` / `systemctl set-property` to pin specific services away from the isolated range.

### 2.4. Advantages over `isolcpus`

- **No reboot required** — adjust CPU assignments at runtime.
- **Reversible** — remove the cgroup or reassign CPUs at any time.
- **Fine-grained** — different cgroups for different workloads.
- **Container-friendly** — works naturally with Docker, Kubernetes, and systemd.

---

## 3. Using Isolated Cores with MTL

Once CPUs are isolated (via either method), assign them to MTL using the `--lcores` option:

```bash
./tests/tools/RxTxApp/build/RxTxApp --config_file config/tx_1v.json --lcores 2,3,4,5
```

Or programmatically via the `lcores` field in `struct mtl_init_params` (see [Design Guide — §2.7 Manual Assigned lcores](design.md#27-manual-assigned-lcores)).

### 3.1. Tasklet Sleep on Non-Isolated Cores

If you cannot dedicate isolated cores, MTL provides options to reduce CPU consumption at the cost of some latency:

| Option | Flag | Effect |
|--------|------|--------|
| `--tasklet_sleep` | `MTL_FLAG_TASKLET_SLEEP` | Sleep when all tasklets report idle. Saves power but adds wake-up latency. |
| `--tasklet_sleep_us <n>` | — | Set the sleep duration in microseconds. |
| `--tasklet_thread` | `MTL_FLAG_TASKLET_THREAD` | Run tasklets as unpinned threads instead of pinned lcores. |

These are useful on shared or busy systems where dedicating isolated cores is not feasible.

### 3.2. Multi-Process Deployment

When running multiple MTL instances, use the **MTL Manager** service to prevent lcore conflicts:

```bash
sudo MtlManager
```

Each MTL instance requests a free core from the Manager. See the [Manager Guide](../manager/README.md) and [Run Guide — §5](run.md#5-run-the-sample-application) for details.

---

## 4. Platform-Specific Notes

### 4.1. Intel Xeon 6 (Granite Rapids) Kernel Bug

> **Warning:** On newer Intel platforms with Xeon 6 CPUs, the `isolcpus` kernel parameter may not be fully respected due to a kernel bug in CPU topology handling. Isolated cores can still receive work from the scheduler, undermining latency-sensitive workloads.
>
> This is fixed in kernel 6.19 by: [sched/fair: Fix imbalance overflow for SD_NUMA domain](https://kernel.googlesource.com/pub/scm/linux/kernel/git/sudeep.holla/linux/+/4d6dd05d07d00bc3bd91183dab4d75caa8018db9).
>
> If running an older kernel on GNR, consider backporting this fix, upgrading your kernel, or using `taskset` to pin processes.

---
```

### Verification Commands

```bash
# Check boot parameters
cat /proc/cmdline | tr ' ' '\n' | grep -E 'isolcpus|nohz'

# Check kernel config for nohz support
grep -E 'CONFIG_NO_HZ_FULL|CONFIG_RCU_FAST_NO_HZ' /boot/config-$(uname -r)

# Check workqueue CPU mask
cat /sys/devices/virtual/workqueue/cpumask

# Check which CPUs are isolated
cat /sys/devices/system/cpu/isolated

# Check per-CPU utilization during stress test
mpstat -P ALL 1

# Check cgroup cpuset assignments
cat /sys/fs/cgroup/mtl/cpuset.cpus 2>/dev/null
```
