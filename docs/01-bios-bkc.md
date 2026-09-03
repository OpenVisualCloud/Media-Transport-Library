# 1. Xeon BIOS configuration

Everything measured later assumes the BIOS settings in this chapter. Hyper-threading
was disabled when benchmarking; it will be tested in a future version. Any deviation
may cause variance in the documented performance results. This is the reference
recipe's BKC (best known configuration) for the platform firmware.

Do this first, before installing anything. The settings themselves need only the
machine's BIOS setup screen — no software, no cluster. The verification script at
the bottom of this page runs either from the control-plane host over SSH (after
[00-before-you-start.md](00-before-you-start.md)) or on the worker itself with
`--local`, which needs nothing configured at all.


**Settings at a glance — three categories:**

| Category | Examples | Where to change |
|---|---|---|
| **BIOS/firmware** | Hyper-Threading, SNC, C-states, Turbo, RDT, memory mode | BIOS setup screen |
| **Linux/runtime** | P-state driver, CPU governor, EPB/EPP/ELC, `/dev/shm` size | `scripts/configure-power.sh` and OS commands |
| **PerfSpect reporting** | Platform identity, BIOS version, DIMM speed | Read-only report — see [04-perfspect-baseline.md](04-perfspect-baseline.md) |

PerfSpect (`scripts/run-perfspect.sh`) runs `perfspect report --all` — it reads and
reports the current configuration. It does not change any settings.

---

## The settings

Apply these to every worker. Hyper-threading was disabled for the documented measurements and is not yet benchmarked. 
The menu paths below are examples using the wording of one common vendor; every vendor 
names these differently and puts them in different submenus. Match on the *effect*, not on the label — and if you cannot
find a setting, `scripts/check-bios.sh` (below) tells you whether the effect is present, which is the only thing that matters.

| # | Setting | Value | Why this reference recipe needs it |
|---|---|---|---|
| 1 | Logical Processor (Hyper-Threading) | **Disabled** | Disabled is the reference state and what the documented numbers were measured on. SMT on is supported, but it has to be declared: `LAB_THREADS_PER_CORE` in `config/lab.env` must match what `lscpu` reports — a mismatch either fails admission with `SMTAlignmentError` or silently hands a container half the cores the scenario claims — see [03-cpu-qos.md § Hyper-Threading](03-cpu-qos.md#hyper-threading). |
| 2 | Sub-NUMA Cluster (SNC) | **Disabled** | The CPU-pool planner and the kubelet's `single-numa-node` Topology Manager both assume one NUMA node per socket. SNC creates 2–4 nodes per socket and the planner cannot model it. |
| 3 | Node Interleaving | **Disabled** | Interleaving spreads every allocation across sockets, making NUMA locality unmeasurable — `numa-pool` and `baseline` would look identical. |
| 4 | Turbo Boost | **Enabled** | The encoder is latency-sensitive at the `medium` preset and needs turbo frequency to hold 60 FPS. |
| 5 | C-States / C1E | **Disabled** (C6 off) | A pinned encoder core idles briefly between frames. C6 exit latency shows up directly as dropped frames. Keep only C0/C1. |
| 6 | System Profile | **Performance** | Stops the BIOS from capping frequency and biases the firmware toward performance. The Linux P-state driver, governor, EPB, EPP and ELC must also be set separately, by `scripts/configure-power.sh` — see below. |
| 7 | Intel RDT / Cache Allocation / Memory Bandwidth Allocation | **Enabled** | Without it `/sys/fs/resctrl` exposes nothing and step 9 (RDT QoS) is impossible. Needed CPU flags: `rdt_a cat_l3 mba cqm_occup_llc cqm_mbm_total cqm_mbm_local`. |
| 8 | Memory operating mode | **Optimizer**, all channels populated | Half-populated channels halve peak DRAM bandwidth and change every bandwidth number in the report without any other visible symptom. |
| 9 | Virtualization / VT-x, VT-d | **Enabled** | Harmless here, required by some container runtimes and by PCM's PCIe access. |

Leave everything else at vendor defaults. Record the BIOS version — it belongs in
the report ([14-reference-bkc.md](14-reference-bkc.md) shows the reference).

### Linux/runtime settings (not BIOS)

These settings must also be correct but are configured in the OS, not in the
BIOS setup screen:

- **CPU governor and energy-performance hints.** `System Profile = Performance` in
  BIOS is necessary but not sufficient — the P-state driver, the governor, EPB, EPP
  and ELC are kernel-side and a reboot resets most of them.
  `scripts/configure-power.sh` sets all five from `config/lab.env` and keeps them —
  see [04-perfspect-baseline.md § Set the power profile](04-perfspect-baseline.md#set-the-power-profile).
- **`/dev/shm` size.** MXL flows live in shared memory under `/dev/shm/mxl`. 20
  streams of v210 need tens of GiB. If `/dev/shm` is too small, remount it:
  `sudo mount -o remount,size=50% /dev/shm`. This is a Linux runtime setting, not
  a BIOS change.

### No BIOS access

If you want SMT off and cannot reach the BIOS setup screen, Linux provides a boot
workaround: the kernel can hide every secondary SMT thread at boot time while
leaving the firmware unchanged. It is optional — SMT on is supported as long as
`LAB_THREADS_PER_CORE` declares it:


Steps 3 and 4 of [00-before-you-start.md](00-before-you-start.md) must be
completed before running `scripts/check-bios.sh` from the control-plane host. Without them you get:

```text
scripts/check-bios.sh: line 19: LAB_SSH_USER: set LAB_SSH_USER in config/nodes.env
```
The `--local` form requires no configuration at all and is useful while you are
still cycling through the BIOS screen.

```bash
scripts/configure-smt-off.sh <worker-node>
sudo reboot                         # run on the worker
scripts/check-bios.sh <worker-node> # after it returns
```

The helper adds `nosmt` via a separate GRUB drop-in and preserves existing kernel
arguments. CPU Manager then sees one online CPU per physical core, which is the
simplest state for `full-pcpus-only`. A reboot is required; toggling
`/sys/devices/system/cpu/smt/control` on a running kubelet is not a valid
measurement setup.

> **`nosmt` and reference measurements.** Using `nosmt` gives the same CPU Manager
> behaviour as disabling SMT in BIOS and is acceptable for reference measurements,
> provided it is noted in the report alongside the BIOS version.

## Verify from Linux

Linux cannot read every BIOS menu value directly. `scripts/check-bios.sh` instead
checks the OS-visible *effects* of those settings — CPU topology, NUMA
configuration, frequency controls, RDT support, DIMM information, and shared-memory
size. It also checks Linux/runtime state (P-state driver, CPU governor, EPB/EPP/ELC,
`/dev/shm`) that is not controlled by the BIOS at all.

The script is **read-only** — it changes nothing.

### What each status means

| Status | Meaning | What to do |
|---|---|---|
| `OK` | The expected configuration was detected. | Nothing. |
| `WARN` | The check could not run yet, or the result is advisory. | Review the message; warnings do not fail the run. |
| `info` | Reported for context only — e.g. EPP/ELC values from PerfSpect when they are readable on this platform. | Nothing; it never fails the run. |
| `WRONG` | The detected configuration does not match the reference recipe. | Correct the BIOS or Linux setting shown in the message. |

The script exits with a non-zero status only when at least one `WRONG` result is
reported. A `WARN` never causes a non-zero exit.


### Option 1: Run from the control-plane host (over SSH)

```bash
# Run from the repository directory on the control-plane host:
scripts/check-bios.sh            # checks LAB_DEFAULT_NODE
scripts/check-bios.sh <node>     # or name a specific worker
```

### Option 2: Run locally on the worker

```bash
# Run directly on the worker — no config, no SSH:
scripts/check-bios.sh --local
```

Both options run exactly the same checks. `--local` skips the inventory and SSH
hop and reports on the machine you are on.

### Sample output

On a correctly configured machine the output looks like this. The system, BIOS, and
CPU lines reflect your hardware; the checks below them are what must pass. This
shows the fully built state — on a fresh host the last two lines are `WARN` instead
(see [Expected first-install warnings](#expected-first-install-warnings) below):

```text
== BIOS BKC check for worker-1 (<worker address>) ==
  System:      <vendor> <model>
  BIOS:        <version> (<date>)
  CPU:         <CPU model>

  OK    Hyper-Threading disabled           1 thread per core, as configured
  OK    Sub-NUMA Clustering disabled       2 NUMA nodes for 2 sockets
  OK    NUMA enabled                       2 nodes
  OK    Turbo Boost enabled                intel_pstate no_turbo=0
  OK    Deep C-states disabled             2 idle states exposed
  OK    P-state driver                     intel_pstate, status=active
  OK    CPU governor                       performance (PerfSpect)
  OK    Energy-performance bias            Performance (0) (PerfSpect-selected source)
  info  Energy-perf preference             Performance (0) (requested 0)
  info  Efficiency Latency Control         Latency Optimized Mode (LOM) (requested latency)
  OK    CPU feature rdt_a                  present
  ...
  OK    L3 CAT ways (cbm_mask)             ffff, min_cbm_bits=1, CLOS=15
  OK    MBA granularity                    min=10 gran=10
  OK    Populated DIMMs                    16 at 6400 MT/s
  OK    /dev/shm size                      252 GiB (MXL flows live here)

BIOS BKC check passed.
```

### What to record

Note the **System**, **BIOS**, and **CPU** lines — they are the platform identity
that every later number is relative to. Full detail is captured automatically by
PerfSpect; see [04-perfspect-baseline.md](04-perfspect-baseline.md) for the
authoritative procedure.

### Expected first-install warnings

On a freshly installed host, before any other chapter has run, these two `WARN`
lines are normal. **Do nothing about them here** — chapter 6 fixes both:

```text
  WARN  resctrl                            not mounted yet - chapter 6 does it (scripts/bootstrap-worker.sh)
  WARN  DIMM population                    DMI needs a sudo rule - chapter 6 adds it (scripts/bootstrap-worker.sh)
```

- **`resctrl` not mounted** — `/sys/fs/resctrl` is the interface through which
  Intel RDT is configured. Nothing mounts it by default. The check that matters at
  this stage is `CPU feature rdt_a / cat_l3 / mba` — if those are `OK`, the
  hardware supports RDT and the mount will work later. If those flags are missing,
  that *is* a BIOS problem (setting 7 in the table above).
- **DIMM population** — the script reads DIMM count and speed using
  `sudo -n dmidecode`, which requires the passwordless rule that chapter 6 installs.
  To see the numbers now, run `sudo dmidecode -t memory` on the worker and enter
  your password.

Two other `WARN` lines are advisory. C-states (`C6+ may add wake-up latency`)
depend on your firmware's granularity — BIOS setting 5. Energy-performance bias is
not a BIOS fix at all — run `scripts/configure-power.sh <node>`. Investigate the
C-state warning only if you cannot reach 60 FPS later.

> **Note on `install-rdt-host.sh` / `install-platform-probe.sh`.** You do not run
> these yourself. They are worker-local root scripts that
> `scripts/bootstrap-worker.sh` — run **from the control-plane host** in
> [06-observability.md](06-observability.md) — copies to the worker and runs there.
> What it stages there is listed in
> [06-observability.md § What bootstrap-worker.sh installs](06-observability.md#what-bootstrap-workersh-installs).

Next: [02-kubernetes-install.md](02-kubernetes-install.md).
