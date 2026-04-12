/**
 * Integration tests — exercise MCP tools against the live /proc and /sys.
 * These tests run on any Linux host (no root required for most).
 */
import { describe, test, expect } from '@jest/globals';
import { capabilities } from '../src/tools/capabilities.js';
import { coreLoadSnapshot } from '../src/tools/core-load-snapshot.js';
import { allowedOnCore } from '../src/tools/allowed-on-core.js';
import { runningOnCore } from '../src/tools/running-on-core.js';
import { irqDistribution } from '../src/tools/irq-distribution.js';
import { softirqSnapshot } from '../src/tools/softirq-snapshot.js';
import { kernelThreadsOnCore } from '../src/tools/kernel-threads-on-core.js';
import { runqueueSnapshot } from '../src/tools/runqueue-snapshot.js';
import { contextSwitchRate } from '../src/tools/context-switch-rate.js';
import { isolationSummary } from '../src/tools/isolation-summary.js';
import { cpuFrequencySnapshot } from '../src/tools/cpu-frequency-snapshot.js';
import { throttlingSummary } from '../src/tools/throttling-summary.js';
import { numaTopology } from '../src/tools/numa-topology.js';
import { processNumaPlacement } from '../src/tools/process-numa-placement.js';
import { cgroupCpuLimits } from '../src/tools/cgroup-cpu-limits.js';
import { perfDebugSnapshot } from '../src/tools/perf-debug-snapshot.js';

// ─── Response envelope checks ──────────────────────────────────────────────

function expectOk(r: any) {
  expect(r.ok).toBe(true);
  expect(r.meta).toBeDefined();
  expect(r.meta.timestamp_wall).toBeDefined();
  expect(typeof r.meta.t_monotonic_ns).toBe('number');
  expect(r.data).toBeDefined();
}

// ─── Capabilities ──────────────────────────────────────────────────────────

describe('capabilities', () => {
  test('returns kernel + CPU info', async () => {
    const r = await capabilities();
    expectOk(r);
    expect(r.data!.cpu_count).toBeGreaterThan(0);
    expect(r.data!.kernel_version).toMatch(/^\d+\.\d+/);
    expect(r.data!.can_read_proc).toBe(true);
    expect(Array.isArray(r.data!.available_tools)).toBe(true);
    expect(r.data!.available_tools.length).toBeGreaterThanOrEqual(19);
  });
});

// ─── core_load_snapshot ────────────────────────────────────────────────────

describe('core_load_snapshot', () => {
  test('returns per-CPU utilization (short window)', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, breakdown: true });
    expectOk(r);
    expect(r.data!.cores.length).toBeGreaterThan(0);
    for (const c of r.data!.cores) {
      expect(typeof c.cpu).toBe('number');
      expect(typeof c.util_pct).toBe('number');
      expect(c.util_pct).toBeGreaterThanOrEqual(0);
      expect(c.util_pct).toBeLessThanOrEqual(100);
    }
  }, 15000);
});

// ─── allowed_on_core ───────────────────────────────────────────────────────

describe('allowed_on_core', () => {
  test('CPU 0 has tasks', async () => {
    const r = await allowedOnCore({ cpu: 0 });
    expectOk(r);
    expect(r.data!.cpu).toBe(0);
    expect(r.data!.task_count).toBeGreaterThan(0);
    expect(r.data!.tasks.length).toBeGreaterThan(0);
    // Every task should have affinity_cpus including CPU 0
    for (const t of r.data!.tasks) {
      expect(t.affinity_cpus).toBeDefined();
      expect(t.affinity_cpus!).toContain(0);
    }
  });

  test('limit param works', async () => {
    const r = await allowedOnCore({ cpu: 0, limit: 5 });
    expectOk(r);
    expect(r.data!.tasks.length).toBeLessThanOrEqual(5);
  });
});

// ─── running_on_core ───────────────────────────────────────────────────────

describe('running_on_core', () => {
  test('observes activity on CPU 0', async () => {
    const r = await runningOnCore({ cpu: 0, window_ms: 200, top_n: 10 });
    expectOk(r);
    expect(r.data!.cpu).toBe(0);
    // May or may not have tasks — depends on workload
    expect(Array.isArray(r.data!.tasks)).toBe(true);
  }, 15000);
});

// ─── irq_distribution ──────────────────────────────────────────────────────

describe('irq_distribution', () => {
  test('returns interrupt deltas', async () => {
    const r = await irqDistribution({ window_ms: 100 });
    expectOk(r);
    // per_cpu: IrqPerCpu[], top_irqs: IrqDetail[]
    expect(Array.isArray(r.data!.per_cpu)).toBe(true);
    expect(Array.isArray(r.data!.top_irqs)).toBe(true);
  }, 15000);
});

// ─── softirq_snapshot ──────────────────────────────────────────────────────

describe('softirq_snapshot', () => {
  test('returns softirq data', async () => {
    const r = await softirqSnapshot({ window_ms: 100 });
    expectOk(r);
    // per_cpu: SoftirqCpuBreakdown[], hot_cpus: number[]
    expect(Array.isArray(r.data!.per_cpu)).toBe(true);
    expect(Array.isArray(r.data!.hot_cpus)).toBe(true);
  }, 15000);
});

// ─── kernel_threads_on_core ────────────────────────────────────────────────

describe('kernel_threads_on_core', () => {
  test('finds kthreads on CPU 0', async () => {
    const r = await kernelThreadsOnCore({ cpu: 0 });
    expectOk(r);
    expect(r.data!.cpu).toBe(0);
    // Linux always has some kthreads; type is KernelThreadsOnCoreData { tasks: RunningTask[] }
    expect(r.data!.tasks.length).toBeGreaterThan(0);
  });
});

// ─── runqueue_snapshot ─────────────────────────────────────────────────────

describe('runqueue_snapshot', () => {
  test('returns loadavg', async () => {
    const r = await runqueueSnapshot();
    expectOk(r);
    // RunqueueSnapshotData { global: RunqueueGlobal, per_cpu?: ... }
    expect(typeof r.data!.global.loadavg_1).toBe('number');
    expect(typeof r.data!.global.loadavg_5).toBe('number');
    expect(typeof r.data!.global.loadavg_15).toBe('number');
    expect(typeof r.data!.global.runnable).toBe('number');
  });
});

// ─── context_switch_rate ───────────────────────────────────────────────────

describe('context_switch_rate', () => {
  test('measures switches', async () => {
    const r = await contextSwitchRate({ window_ms: 100 });
    expectOk(r);
    // ContextSwitchRateData { global_ctxt_per_s: number, per_cpu?: ... }
    expect(typeof r.data!.global_ctxt_per_s).toBe('number');
    expect(r.data!.global_ctxt_per_s).toBeGreaterThan(0);
  }, 15000);
});

// ─── isolation_summary ─────────────────────────────────────────────────────

describe('isolation_summary', () => {
  test('reads kernel cmdline', async () => {
    const r = await isolationSummary();
    expectOk(r);
    // IsolationSummaryData { cmdline_flags, isolated_cpus, ... }
    expect(r.data!.cmdline_flags).toBeDefined();
    expect(Array.isArray(r.data!.isolated_cpus)).toBe(true);
  });
});

// ─── cpu_frequency_snapshot ────────────────────────────────────────────────

describe('cpu_frequency_snapshot', () => {
  test('returns frequency info', async () => {
    const r = await cpuFrequencySnapshot();
    expectOk(r);
    // CpuFrequencySnapshotData { cpus: CpuFrequencyEntry[] }
    expect(Array.isArray(r.data!.cpus)).toBe(true);
  });
});

// ─── throttling_summary ────────────────────────────────────────────────────

describe('throttling_summary', () => {
  test('returns throttle data', async () => {
    const r = await throttlingSummary({ window_ms: 100 });
    expectOk(r);
    // ThrottlingSummaryData { thermal_throttle_count?, notes }
    expect(Array.isArray(r.data!.notes)).toBe(true);
  }, 15000);
});

// ─── numa_topology ─────────────────────────────────────────────────────────

describe('numa_topology', () => {
  test('returns nodes and CPUs', async () => {
    const r = await numaTopology();
    expectOk(r);
    // NumaTopologyData { nodes: NumaNode[], distances? }
    expect(r.data!.nodes.length).toBeGreaterThan(0);
    for (const n of r.data!.nodes) {
      expect(Array.isArray(n.cpus)).toBe(true);
      expect(n.cpus.length).toBeGreaterThan(0);
    }
  });
});

// ─── process_numa_placement ────────────────────────────────────────────────

describe('process_numa_placement', () => {
  test('PID 1 (init)', async () => {
    const r = await processNumaPlacement({ target_pid: 1 });
    expectOk(r);
    expect(r.data!.pid).toBe(1);
    expect(Array.isArray(r.data!.cpus_allowed_list)).toBe(true);
    expect(Array.isArray(r.data!.mems_allowed_list)).toBe(true);
  });
});

// ─── cgroup_cpu_limits ─────────────────────────────────────────────────────

describe('cgroup_cpu_limits', () => {
  test('reads cgroup for PID 1', async () => {
    const r = await cgroupCpuLimits({ target_pid: 1 });
    // May fail if cgroup v2 not mounted — that's ok
    if (r.ok) {
      expectOk(r);
    } else {
      expect(r.error).toBeDefined();
    }
  });
});

// ─── perf_debug_snapshot (aggregator) ──────────────────────────────────────

describe('perf_debug_snapshot', () => {
  test('returns aggregated data', async () => {
    const r = await perfDebugSnapshot({ window_ms: 100 });
    expectOk(r);
    expect(r.data!.core_load).toBeDefined();
    expect(r.data!.irq_distribution).toBeDefined();
    expect(r.data!.softirq_snapshot).toBeDefined();
    expect(r.data!.cpu_frequency).toBeDefined();
    expect(r.data!.isolation).toBeDefined();
  }, 30000);
});
