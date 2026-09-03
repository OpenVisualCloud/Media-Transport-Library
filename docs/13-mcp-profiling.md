# 13. MCP profiling — deep-dive diagnostics

When a DMF stream misses its frame deadline, the campaign reports show an FPS
drop. They do not tell you which core was stolen by what. That is what this is
for.

`cpu-debug-mcp` is an MCP server exposing ~70 **read-only** Linux CPU and perf
tools — per-core load, thread affinity, starvation analysis, IRQ and softirq
distribution, cgroup quota accounting, NUMA placement, Intel PCM counters,
turbostat frequency and C-state residency, and eBPF latency tracing. An AI agent
(or you, through any MCP client) can ask the worker questions in plain language
instead of remembering fifteen tool invocations.

## Install

```bash
scripts/install-mcp-profiler.sh              # LAB_DEFAULT_NODE
scripts/install-mcp-profiler.sh <node>
```

Sources: a local checkout at `MCP_LOCAL_DIR` (default `../cpu-debug-MCP`, copied
with rsync) wins; otherwise `MCP_REPO` is cloned on the worker. The script
installs Node ≥ 18 if needed, adds the optional tool packages (`sysstat`,
`ethtool`, `linux-tools-*`, `bpfcc-tools`, `bpftrace` — each degrades gracefully
if unavailable), builds the server, and runs it as
`cpu-debug-mcp.service` bound to **loopback only** with `CPUAffinity` set to the
reserved CPUs.

## Connect

```bash
ssh -N -L 3001:127.0.0.1:3001 <user>@<worker>
```

Then point your client at it (`.vscode/mcp.json`, or your agent's config):

```json
{
  "servers": {
    "cpu-debug": { "type": "http", "url": "http://127.0.0.1:3001/mcp" }
  }
}
```

Loopback plus SSH tunnel is deliberate: the tool surface is never exposed on the
lab network. Call the **`capabilities`** tool first — it reports which optional
subsystems (PCM on 9738, EMON, BCC, turbostat) this particular worker actually
has.

For stdio clients, run `node dist/index.js` on the worker without `--tcp`.

## Use it on a *repro* run, not a published one

The tools are read-only, but tracing perturbs the system: eBPF probes and
`perf`-based sampling cost cycles, and on a machine that is 97 % busy that cost
shows up as dropped frames. So:

1. Run the campaign clean. Record the numbers.
2. Reproduce the failure with `--keep` so the Pods stay up.
3. Profile that instance.

```bash
scripts/run.sh pinned --streams 22 --keep --warmup 1m --measure 2m
# ... investigate while it is running or after it finishes ...
scripts/teardown.sh
```

## Triage recipes

Ask these in the order the failure modes actually occur.

**"The FPS is low but CPU looks idle."** → per-core load and starvation analysis.
Look for one saturated core among many idle ones: that is a serialised thread
(usually too few slices, or a decoder bottleneck starving its encoder), not a
capacity problem.

**"Which core is being stolen?"** → per-core utilisation cross-referenced with
each container's cpuset. Anything running on an exclusive encoder core that is not
that encoder is the culprit: a host process (see `host-a` in
[11-noisy-neighbor.md](11-noisy-neighbor.md)), a DaemonSet that escaped its
cgroup, or IRQ handling.

**"Is it interrupts?"** → IRQ and softirq distribution per CPU. A NIC queue pinned
to an encoder core will cost frames. Move the affinity, or move the encoder.

**"Is it frequency?"** → turbostat residency. If busy cores are not at expected
turbo, or C-state residency is non-zero on pinned cores, the BIOS settings from
[01-bios-bkc.md](01-bios-bkc.md) have drifted — check the governor first, it is
the usual culprit after a reboot.

**"Is it NUMA?"** → thread placement versus memory locality. Cross-socket UPI in
the summary tells you traffic exists; this tells you which threads are causing it.

**"Is it throttling?"** → cgroup quota and throttled-time counters. In `baseline`
the Burstable Pods have no CPU limit, so this should be zero; if it is not,
something set a limit.

**"Is it the cache?"** → PCM counters and RDT occupancy per group. The summary
already gives you whole-worker L3 hit ratio and per-group occupancy; use this for
the per-core breakdown behind it.

## How it fits the DMF workflow

| Question | Tool |
|---|---|
| Is the platform configured correctly? | PerfSpect ([04-perfspect-baseline.md](04-perfspect-baseline.md)) |
| How many DMF streams fit? | the campaign runner ([09-density.md](09-density.md)) |
| What did the whole worker do during the run? | Prometheus and Grafana ([06-observability.md](06-observability.md)) |
| Who took the cache and bandwidth? | RDT ([12-rdt-qos.md](12-rdt-qos.md)) |
| What resources does a container actually need? | this, plus [08-profiling-manifest.md](08-profiling-manifest.md) |
| **Why did this specific DMF stream miss its deadline?** | **this** |

Next: [14-reference-bkc.md](14-reference-bkc.md) for the platform all the numbers
came from, and [10-metrics.md](10-metrics.md) for what every column means.
