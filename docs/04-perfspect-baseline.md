# 4. PerfSpect baseline report

Capture the platform once, before any measurement. Set the power profile first
([Set the power profile](#set-the-power-profile)) so the baseline records the
configured state, not the default one.

> If this environment uses an outbound proxy, set `LAB_HTTP_PROXY`/`LAB_HTTPS_PROXY`
> in `config/lab.env` first. Scripts build `NO_PROXY` for cluster/node addresses, but
> manual `kubectl` commands may still need shell `no_proxy`/`NO_PROXY`.

## Run it

```bash
scripts/run-perfspect.sh              # LAB_DEFAULT_NODE
scripts/run-perfspect.sh <node>       # a specific worker
```

Three stages: install [Intel PerfSpect](https://github.com/intel/PerfSpect) on
the worker if it is not there, run the full configuration report as root, copy it
back to the controller.

Output lands in:

```
results/perfspect/<node>/baseline-<UTC timestamp>/     the report (html, json, xlsx)
results/perfspect/<node>/latest                        symlink to the newest
```

PerfSpect reads MSRs and DMI, so it needs root on the worker — you will be
prompted for the worker's `sudo` password on the worker's own prompt.

## What to check in the report

| Report section | What it must say | Why |
|---|---|---|
| BIOS | version and date recorded | The comparison baseline. |
| CPU | model, sockets, cores, `Hyper-Threading` (reference: `Disabled`) | Confirms step 1; must match `LAB_THREADS_PER_CORE` in `config/lab.env`. |
| NUMA | one node per socket | Confirms SNC off. |
| Frequency | base, all-core turbo, `Turbo: Enabled` | Encoder throughput scales with all-core turbo, so a different turbo bin changes the density result legitimately. |
| Power | `System Profile: Performance`, governor `performance`, EPB 0 | A `powersave` governor typically costs 1–2 streams. |
| C-states | only C0/C1 exposed | C6 exit latency causes dropped frames on pinned cores. |
| Prefetchers | all enabled (default) | Disabled prefetchers change L3 hit ratio and DRAM traffic. |
| Uncore | max/min frequency | Affects the DRAM and UPI numbers directly. |
| Memory | DIMM count, size, **type and configured speed**, channels populated | The theoretical DRAM peak in the summary is `channels × MT/s × 8`. A half-populated machine halves it. |
| Kernel / OS | version, `isolcpus`/`nohz_full` if any | Reference: Ubuntu 24.04.4, kernel 6.17.0-19-generic. |
| PMU | not in use by another agent | PCM cannot read uncore counters if something else holds the PMU. |
| Accelerators | — | Unused here; H.264 encoding is pure software (libx264). |

## Set the power profile

The report above only *reads*. Five power/frequency controls decide how much
frequency the cores a run is given actually reach, and none of them is visible
in the result as anything but a lower stream count:

| Setting | Reference value | Where it lives | Why it changes the number |
|---|---|---|---|
| P-state driver | `intel_pstate=active` | kernel argument + `/sys/devices/system/cpu/intel_pstate/status` | `active` is HWP: the hardware picks the P-state from EPP. `passive` hands control to the governor instead. Different control loop, different frequency under a bursty encoder. |
| Scaling governor | `performance` | `cpu*/cpufreq/scaling_governor` | `powersave` ramps down between frames. Typically costs 1–2 streams. |
| EPB (PerfSpect-selected source) | `0` | PerfSpect `config` report (`Energy Performance Bias`) | The coarse BIOS/OS hint, 0 = performance, 15 = maximum saving. PerfSpect selects the active source and reports the effective value. |
| EPP (MSR 0x774) | `0` | `cpu*/cpufreq/energy_performance_preference` | The HWP hint, 0 = performance. Only exists while the driver is in `active` mode. |
| ELC | `latency` | TPMI, PerfSpect only | Efficiency Latency Control, Sierra Forest and newer. Decides how eagerly the part trades frequency for efficiency at low utilisation. |

`scripts/configure-power.sh` applies all five, from the
`worker power and frequency` block of `config/lab.env`, using PerfSpect for the
MSRs and sysfs for the driver:

```bash
scripts/configure-power.sh                    # LAB_DEFAULT_NODE
scripts/configure-power.sh <node>             # a specific worker
scripts/configure-power.sh --verify <node>    # read back only, change nothing
```

Five stages: install PerfSpect if absent, **record the current configuration**,
switch the P-state driver, apply governor/EPB/EPP/ELC, and install the unit that
re-applies them at boot. It ends by reading back P-state from sysfs and
governor/EPB/EPP/ELC from PerfSpect `config`, and exits non-zero if required
settings did not take.


Not every setting exists on every part. `--elc` needs Sierra Forest or newer and
`--epp` needs the driver in `active` mode; each is applied on its own so an
unsupported one cannot take the others down with it. Both report what happened and
leave the rest in place. Set `LAB_POWER_ELC=skip` or `LAB_POWER_PSTATE_DRIVER=skip`
to stop trying.

## Using it as evidence

Two rules that make the reports comparable:

1. **Capture before the workload**, not during. This is a configuration report,
   not a profile. Live behaviour comes from Prometheus
   ([06-observability.md](06-observability.md)) and the MCP profiler
   ([13-mcp-profiling.md](13-mcp-profiling.md)).
2. **Recapture after any BIOS or kernel change** and keep both. The timestamped
   directory names make the history readable.

Next: [05-ffmpeg-mxl-container.md](05-ffmpeg-mxl-container.md).
