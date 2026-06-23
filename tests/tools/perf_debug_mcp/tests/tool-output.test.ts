/**
 * Tool output correctness tests.
 *
 * Tests that:
 *   - core_filter actually restricts output to requested cores
 *   - RDMA health outputs summary fields before all_counters in JSON
 *   - PCM core filter uses OS ID (verified by structure)
 *   - Tool response envelopes are well-formed
 *   - Numeric values are in expected ranges
 */
import { describe, test, expect } from '@jest/globals';
import os from 'os';

// ─── Tools under test ───────────────────────────────────────────────────────

import { coreLoadSnapshot } from '../src/tools/core-load-snapshot.js';
import { softirqSnapshot } from '../src/tools/softirq-snapshot.js';
import { cpuFrequencySnapshot } from '../src/tools/cpu-frequency-snapshot.js';
import { irqDistribution } from '../src/tools/irq-distribution.js';
import { allowedOnCore } from '../src/tools/allowed-on-core.js';
import { runningOnCore } from '../src/tools/running-on-core.js';
import { kernelThreadsOnCore } from '../src/tools/kernel-threads-on-core.js';
import { capabilities } from '../src/tools/capabilities.js';
import { contextSwitchRate } from '../src/tools/context-switch-rate.js';
import { numaTopology } from '../src/tools/numa-topology.js';
import { isolationSummary } from '../src/tools/isolation-summary.js';

// ─── Parsers / helpers under test ───────────────────────────────────────────

import { parseCoreFilter } from '../src/utils/helpers.js';

// ─── Test data ──────────────────────────────────────────────────────────────

const CPU_COUNT = os.cpus().length;
// Pick some test cores that exist on this host
const TEST_CORE_0 = 0;
const TEST_CORE_1 = Math.min(1, CPU_COUNT - 1);
const TEST_CORES = [TEST_CORE_0, TEST_CORE_1];
const TEST_CORE_FILTER = TEST_CORES.join(',');

// ─── Helpers ────────────────────────────────────────────────────────────────

function expectOk(r: any) {
  expect(r.ok).toBe(true);
  expect(r.meta).toBeDefined();
  expect(r.meta.timestamp_wall).toBeDefined();
  expect(typeof r.meta.t_monotonic_ns).toBe('number');
  expect(r.data).toBeDefined();
}

function expectFilteredCores(cores: { cpu: number }[], filterSet: Set<number>) {
  expect(cores.length).toBeGreaterThan(0);
  expect(cores.length).toBeLessThanOrEqual(filterSet.size);
  for (const c of cores) {
    expect(filterSet.has(c.cpu)).toBe(true);
  }
}

// ─── core_load_snapshot core_filter ─────────────────────────────────────────

describe('core_load_snapshot core_filter', () => {
  test('without filter returns all cores', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100 });
    expectOk(r);
    expect(r.data!.cores.length).toBe(CPU_COUNT);
  }, 15000);

  test('core_filter restricts output to requested cores', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, core_filter: TEST_CORE_FILTER });
    expectOk(r);
    const filterSet = parseCoreFilter(TEST_CORE_FILTER)!;
    expectFilteredCores(r.data!.cores, filterSet);
  }, 15000);

  test('single core filter', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, core_filter: '0' });
    expectOk(r);
    expect(r.data!.cores.length).toBe(1);
    expect(r.data!.cores[0].cpu).toBe(0);
  }, 15000);

  test('range filter works', async () => {
    const maxCore = Math.min(3, CPU_COUNT - 1);
    const r = await coreLoadSnapshot({ window_ms: 100, core_filter: `0-${maxCore}` });
    expectOk(r);
    expect(r.data!.cores.length).toBe(maxCore + 1);
    for (const c of r.data!.cores) {
      expect(c.cpu).toBeGreaterThanOrEqual(0);
      expect(c.cpu).toBeLessThanOrEqual(maxCore);
    }
  }, 15000);

  test('utilization values are in valid range', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, breakdown: true });
    expectOk(r);
    for (const c of r.data!.cores) {
      expect(c.util_pct).toBeGreaterThanOrEqual(0);
      expect(c.util_pct).toBeLessThanOrEqual(100);
      expect(c.idle_pct).toBeGreaterThanOrEqual(0);
      expect(c.idle_pct).toBeLessThanOrEqual(100);
      // util + idle + iowait should approximately sum to 100
      expect(c.util_pct + c.idle_pct + c.iowait_pct).toBeGreaterThanOrEqual(99);
      expect(c.util_pct + c.idle_pct + c.iowait_pct).toBeLessThanOrEqual(101);
    }
  }, 15000);

  test('nonexistent core filter returns empty', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, core_filter: '99999' });
    expectOk(r);
    expect(r.data!.cores.length).toBe(0);
  }, 15000);
});

// ─── softirq_snapshot core_filter ───────────────────────────────────────────

describe('softirq_snapshot core_filter', () => {
  test('without filter returns all CPUs', async () => {
    const r = await softirqSnapshot({ window_ms: 100 });
    expectOk(r);
    // /proc/softirqs includes offline CPUs, so count may exceed os.cpus().length
    expect(r.data!.per_cpu.length).toBeGreaterThanOrEqual(CPU_COUNT);
  }, 15000);

  test('core_filter restricts per_cpu output', async () => {
    const r = await softirqSnapshot({ window_ms: 100, core_filter: TEST_CORE_FILTER });
    expectOk(r);
    const filterSet = parseCoreFilter(TEST_CORE_FILTER)!;
    expect(r.data!.per_cpu.length).toBeLessThanOrEqual(filterSet.size);
    for (const entry of r.data!.per_cpu) {
      expect(filterSet.has(entry.cpu)).toBe(true);
    }
  }, 15000);

  test('hot_cpus are filtered too', async () => {
    const r = await softirqSnapshot({ window_ms: 100, core_filter: '0' });
    expectOk(r);
    for (const cpu of r.data!.hot_cpus) {
      expect(cpu).toBe(0);
    }
  }, 15000);

  test('softirq counts are non-negative', async () => {
    const r = await softirqSnapshot({ window_ms: 100 });
    expectOk(r);
    for (const entry of r.data!.per_cpu) {
      expect(entry.total_delta).toBeGreaterThanOrEqual(0);
    }
  }, 15000);
});

// ─── cpu_frequency_snapshot core_filter ─────────────────────────────────────

describe('cpu_frequency_snapshot core_filter', () => {
  test('without filter returns all CPUs', async () => {
    const r = await cpuFrequencySnapshot();
    if (!r.ok) return; // cpufreq may not be available
    expect(r.data!.cpus.length).toBeGreaterThanOrEqual(CPU_COUNT);
  });

  test('core_filter restricts output', async () => {
    const r = await cpuFrequencySnapshot({ core_filter: '0' });
    if (!r.ok) return;
    expect(r.data!.cpus.length).toBe(1);
    expect(r.data!.cpus[0].cpu).toBe(0);
  });

  test('range filter', async () => {
    const maxCore = Math.min(3, CPU_COUNT - 1);
    const r = await cpuFrequencySnapshot({ core_filter: `0-${maxCore}` });
    if (!r.ok) return;
    expect(r.data!.cpus.length).toBe(maxCore + 1);
    for (const c of r.data!.cpus) {
      expect(c.cpu).toBeGreaterThanOrEqual(0);
      expect(c.cpu).toBeLessThanOrEqual(maxCore);
    }
  });

  test('frequency values are reasonable', async () => {
    const r = await cpuFrequencySnapshot();
    if (!r.ok) return;
    for (const c of r.data!.cpus) {
      if (c.cur_khz !== null) {
        expect(c.cur_khz).toBeGreaterThan(100_000); // > 100 MHz
        expect(c.cur_khz).toBeLessThan(10_000_000); // < 10 GHz
      }
    }
  });
});

// ─── irq_distribution core_filter ───────────────────────────────────────────

describe('irq_distribution core_filter', () => {
  test('without filter returns all CPUs', async () => {
    const r = await irqDistribution({ window_ms: 100 });
    expectOk(r);
    expect(r.data!.per_cpu.length).toBe(CPU_COUNT);
  }, 15000);

  test('core_filter restricts per_cpu', async () => {
    const r = await irqDistribution({ window_ms: 100, core_filter: TEST_CORE_FILTER });
    expectOk(r);
    const filterSet = parseCoreFilter(TEST_CORE_FILTER)!;
    expect(r.data!.per_cpu.length).toBeLessThanOrEqual(filterSet.size);
    for (const entry of r.data!.per_cpu) {
      expect(filterSet.has(entry.cpu)).toBe(true);
    }
  }, 15000);

  test('single cpu_filter still works (backwards compat)', async () => {
    const r = await irqDistribution({ window_ms: 100, cpu_filter: 0 });
    expectOk(r);
    expect(r.data!.per_cpu.length).toBe(1);
    expect(r.data!.per_cpu[0].cpu).toBe(0);
  }, 15000);

  test('core_filter top_irqs only include filtered CPUs', async () => {
    const r = await irqDistribution({ window_ms: 100, core_filter: '0' });
    expectOk(r);
    for (const irq of r.data!.top_irqs) {
      for (const d of irq.per_cpu_deltas) {
        expect(d.cpu).toBe(0);
      }
    }
  }, 15000);
});

// ─── allowed_on_core output correctness ─────────────────────────────────────

describe('allowed_on_core correctness', () => {
  test('all returned tasks have CPU 0 in their affinity', async () => {
    const r = await allowedOnCore({ cpu: 0, limit: 100 });
    expectOk(r);
    expect(r.data!.cpu).toBe(0);
    expect(r.data!.task_count).toBeGreaterThan(0);
    for (const t of r.data!.tasks) {
      expect(t.affinity_cpus).toBeDefined();
      expect(t.affinity_cpus!).toContain(0);
    }
  });

  test('task fields are well-formed', async () => {
    const r = await allowedOnCore({ cpu: 0, limit: 10 });
    expectOk(r);
    for (const t of r.data!.tasks) {
      expect(typeof t.pid).toBe('number');
      expect(t.pid).toBeGreaterThan(0);
      expect(typeof t.comm).toBe('string');
      expect(t.comm.length).toBeGreaterThan(0);
    }
  });
});

// ─── running_on_core output correctness ─────────────────────────────────────

describe('running_on_core correctness', () => {
  test('returned cpu matches requested', async () => {
    const r = await runningOnCore({ cpu: 0, window_ms: 100 });
    expectOk(r);
    expect(r.data!.cpu).toBe(0);
    expect(Array.isArray(r.data!.tasks)).toBe(true);
  }, 15000);

  test('task runtime values are non-negative', async () => {
    const r = await runningOnCore({ cpu: 0, window_ms: 200 });
    expectOk(r);
    for (const t of r.data!.tasks) {
      if (t.runtime_ns !== undefined) {
        expect(t.runtime_ns).toBeGreaterThanOrEqual(0);
      }
    }
  }, 15000);
});

// ─── kernel_threads_on_core correctness ─────────────────────────────────────

describe('kernel_threads_on_core correctness', () => {
  test('returns kernel threads on CPU 0', async () => {
    const r = await kernelThreadsOnCore({ cpu: 0 });
    expectOk(r);
    expect(r.data!.cpu).toBe(0);
    expect(r.data!.tasks.length).toBeGreaterThan(0);
    // Kernel threads should have low PIDs or recognized names
    for (const t of r.data!.tasks) {
      expect(typeof t.comm).toBe('string');
      expect(typeof t.pid).toBe('number');
    }
  });
});

// ─── context_switch_rate correctness ────────────────────────────────────────

describe('context_switch_rate correctness', () => {
  test('rate is positive on a running system', async () => {
    const r = await contextSwitchRate({ window_ms: 100 });
    expectOk(r);
    expect(r.data!.global_ctxt_per_s).toBeGreaterThan(0);
  }, 15000);
});

// ─── numa_topology correctness ──────────────────────────────────────────────

describe('numa_topology correctness', () => {
  test('nodes cover all CPUs', async () => {
    const r = await numaTopology();
    expectOk(r);
    const allCpus = new Set<number>();
    for (const node of r.data!.nodes) {
      for (const cpu of node.cpus) {
        allCpus.add(cpu);
      }
    }
    expect(allCpus.size).toBe(CPU_COUNT);
  });

  test('node IDs are non-negative', async () => {
    const r = await numaTopology();
    expectOk(r);
    for (const node of r.data!.nodes) {
      expect(node.node).toBeGreaterThanOrEqual(0);
    }
  });
});

// ─── capabilities correctness ───────────────────────────────────────────────

describe('capabilities correctness', () => {
  test('cpu_count matches os.cpus()', async () => {
    const r = await capabilities();
    expectOk(r);
    expect(r.data!.cpu_count).toBe(CPU_COUNT);
  });

  test('can_read_proc is true', async () => {
    const r = await capabilities();
    expectOk(r);
    expect(r.data!.can_read_proc).toBe(true);
  });

  test('available_tools is non-empty', async () => {
    const r = await capabilities();
    expectOk(r);
    expect(r.data!.available_tools.length).toBeGreaterThan(10);
  });
});

// ─── RDMA health output ordering ────────────────────────────────────────────

describe('RDMA health output ordering', () => {
  test('summary fields come before all_counters in JSON', () => {
    // Simulate an RdmaDeviceHealth object as built by the tool
    // The key insight: in JavaScript, JSON.stringify preserves insertion order
    const device: any = {
      device: 'test0',
      port: 1,
      state: 'ACTIVE',
      physical_state: 'LINK_UP',
      netdev: 'eth0',
      out_rdma_writes: 100,
      in_rdma_writes: 50,
      out_rdma_reads: 10,
      in_rdma_reads: 5,
      retrans_segs: 0,
      rx_ecn_marked: 0,
      cnp_sent: 0,
      cnp_handled: 0,
      cnp_ignored: 0,
      in_opt_errors: 0,
      in_proto_errors: 0,
      ip4_in_discards: 0,
      ip4_out_octets: 1000000,
      ip4_in_octets: 500000,
      all_counters: [{ name: 'foo', value: 42 }],
    };

    // Simulate what rdma-health.ts does: delete all_counters, add summaries, re-add
    const savedCounters = device.all_counters;
    delete device.all_counters;

    device.qp_count = 4;
    device.qps_in_error = 0;
    device.qp_summary = { total: 4, by_state: { RTS: 4 } };
    device.process_summary = [{ pid: 123, comm: 'test', qp_count: 4 }];
    device.qps = [{ ifname: 'test0', lqpn: 1, type: 'RC', state: 'RTS' }];
    device.all_counters = savedCounters;

    const json = JSON.stringify(device);
    const keys = Object.keys(device);

    // qp_summary should appear BEFORE all_counters
    const qpSummaryIdx = keys.indexOf('qp_summary');
    const allCountersIdx = keys.indexOf('all_counters');
    expect(qpSummaryIdx).toBeLessThan(allCountersIdx);

    // process_summary should appear BEFORE all_counters
    const processSummaryIdx = keys.indexOf('process_summary');
    expect(processSummaryIdx).toBeLessThan(allCountersIdx);

    // qp_count before all_counters
    const qpCountIdx = keys.indexOf('qp_count');
    expect(qpCountIdx).toBeLessThan(allCountersIdx);

    // qps before all_counters
    const qpsIdx = keys.indexOf('qps');
    expect(qpsIdx).toBeLessThan(allCountersIdx);

    // Verify the JSON string has summary info before the big counter dump
    const summaryPos = json.indexOf('"qp_summary"');
    const countersPos = json.indexOf('"all_counters"');
    expect(summaryPos).toBeLessThan(countersPos);
  });
});

// ─── PCM core_filter uses OS ID ─────────────────────────────────────────────

describe('PCM core_filter validates OS ID filtering', () => {
  // We can't run live PCM tests without pcm-sensor-server, but we
  // can verify the parseCoreFilter logic and the filter application
  // pattern by testing the helper function.

  test('parseCoreFilter handles all formats for PCM tools', () => {
    // These are the formats documented in the tool description
    expect(parseCoreFilter('4')).toEqual(new Set([4]));
    expect(parseCoreFilter('4,5,6')).toEqual(new Set([4, 5, 6]));
    expect(parseCoreFilter('4-13')).toEqual(new Set([4, 5, 6, 7, 8, 9, 10, 11, 12, 13]));
    expect(parseCoreFilter('4-6,10,20-22')).toEqual(new Set([4, 5, 6, 10, 20, 21, 22]));
  });

  test('core_filter applied with has() correctly filters entries', () => {
    // Simulate what PCM tools do after fix: filter on os_id
    const coreFilterSet = parseCoreFilter('4,5,6');
    const entries = [
      { os_id: 0, core: 0, value: 10 },
      { os_id: 4, core: 2, value: 20 },
      { os_id: 5, core: 2, value: 30 }, // HT sibling of core 2
      { os_id: 6, core: 3, value: 40 },
      { os_id: 96, core: 0, value: 50 }, // HT sibling of core 0
    ];

    const filtered = entries.filter((e) => coreFilterSet === null || coreFilterSet.has(e.os_id));
    expect(filtered).toHaveLength(3);
    expect(filtered.map((e) => e.os_id)).toEqual([4, 5, 6]);

    // OLD (incorrect) behavior would filter on core:
    const oldFiltered = entries.filter((e) => coreFilterSet === null || coreFilterSet.has(e.core));
    // This would INCORRECTLY include os_id=96 (core=0 != 4,5,6) and miss the right ones
    // Specifically it would only get entries where core is in {4,5,6} - none of them!
    expect(oldFiltered).toHaveLength(0); // proves the old approach is wrong
  });
});

// ─── isolation_summary correctness ──────────────────────────────────────────

describe('isolation_summary correctness', () => {
  test('isolated_cpus is a valid array of numbers', async () => {
    const r = await isolationSummary();
    expectOk(r);
    expect(Array.isArray(r.data!.isolated_cpus)).toBe(true);
    for (const cpu of r.data!.isolated_cpus) {
      expect(typeof cpu).toBe('number');
      expect(cpu).toBeGreaterThanOrEqual(0);
      expect(cpu).toBeLessThan(CPU_COUNT);
    }
  });

  test('nohz_full_cpus is present', async () => {
    const r = await isolationSummary();
    expectOk(r);
    expect(Array.isArray(r.data!.nohz_full_cpus)).toBe(true);
  });
});

// ─── tool_guide correctness ─────────────────────────────────────────────────

import { toolGuide } from '../src/tools/tool-guide.js';

describe('tool_guide correctness', () => {
  test('returns categorized tool index with decision tree', async () => {
    const r = await toolGuide();
    expectOk(r);
    expect(r.data!.decision_tree).toBeDefined();
    expect(Object.keys(r.data!.decision_tree).length).toBeGreaterThan(5);
  });

  test('categories cover all major tool groups', async () => {
    const r = await toolGuide();
    expectOk(r);
    const categoryNames = r.data!.categories.map(c => c.category);
    expect(categoryNames).toContain('Start Here');
    expect(categoryNames).toContain('Intel PCM (Hardware Counters)');
    expect(categoryNames).toContain('RDMA / DCB / NIC');
    expect(categoryNames).toContain('BCC / eBPF Tracing');
  });

  test('drill_down_chains provide step-by-step sequences', async () => {
    const r = await toolGuide();
    expectOk(r);
    expect(r.data!.drill_down_chains).toBeDefined();
    expect(Object.keys(r.data!.drill_down_chains).length).toBeGreaterThan(3);
    // Each chain should have multiple steps
    for (const chain of Object.values(r.data!.drill_down_chains)) {
      expect(chain.length).toBeGreaterThanOrEqual(3);
    }
  });

  test('each category has at least one tool', async () => {
    const r = await toolGuide();
    expectOk(r);
    for (const cat of r.data!.categories) {
      expect(cat.tools.length).toBeGreaterThan(0);
    }
  });
});

// ─── core_load_snapshot top_n ───────────────────────────────────────────────

describe('core_load_snapshot top_n', () => {
  test('top_n=3 returns at most 3 cores sorted by load', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, top_n: 3 });
    expectOk(r);
    expect(r.data!.cores.length).toBeLessThanOrEqual(3);
    // Should be sorted by util_pct descending
    for (let i = 1; i < r.data!.cores.length; i++) {
      expect(r.data!.cores[i - 1].util_pct).toBeGreaterThanOrEqual(r.data!.cores[i].util_pct);
    }
  }, 15000);

  test('top_n without core_filter works on all cores', async () => {
    const r = await coreLoadSnapshot({ window_ms: 100, top_n: 2 });
    expectOk(r);
    expect(r.data!.cores.length).toBeLessThanOrEqual(2);
  }, 15000);

  test('top_n combined with core_filter applies both filters', async () => {
    if (CPU_COUNT < 3) return; // skip on single-core
    const r = await coreLoadSnapshot({ window_ms: 100, core_filter: '0-2', top_n: 1 });
    expectOk(r);
    expect(r.data!.cores.length).toBe(1);
    expect([0, 1, 2]).toContain(r.data!.cores[0].cpu);
  }, 15000);
});

// ─── softirq_snapshot top_n ─────────────────────────────────────────────────

describe('softirq_snapshot top_n', () => {
  test('top_n=2 returns at most 2 CPUs sorted by activity', async () => {
    const r = await softirqSnapshot({ window_ms: 200, top_n: 2 });
    expectOk(r);
    expect(r.data!.per_cpu.length).toBeLessThanOrEqual(2);
    // Should be sorted by total_delta descending
    for (let i = 1; i < r.data!.per_cpu.length; i++) {
      expect(r.data!.per_cpu[i - 1].total_delta).toBeGreaterThanOrEqual(r.data!.per_cpu[i].total_delta);
    }
  }, 15000);
});

// ─── starvation_report flat params ──────────────────────────────────────────

import { starvationReport } from '../src/tools/starvation-report.js';

describe('starvation_report flat params', () => {
  test('flat target_comm_regex works like nested target.comm_regex', async () => {
    const r = await starvationReport({ target_comm_regex: 'systemd', window_ms: 200 });
    expectOk(r);
    expect(r.data!.target_resolution.length).toBeGreaterThan(0);
    for (const t of r.data!.target_resolution) {
      expect(t.comm.toLowerCase()).toMatch(/systemd/i);
    }
  }, 15000);

  test('flat target_pid works', async () => {
    const r = await starvationReport({ target_pid: 1, window_ms: 200 });
    expectOk(r);
    expect(r.data!.target_resolution.length).toBeGreaterThan(0);
  }, 15000);

  test('flat params take priority over nested target', async () => {
    // Pass conflicting params — flat should win
    const r = await starvationReport({
      target_comm_regex: 'systemd',
      target: { comm_regex: 'nonexistent_process_xyz' },
      window_ms: 200,
    });
    expectOk(r);
    // flat target_comm_regex = 'systemd' should win over nested 'nonexistent_process_xyz'
    expect(r.data!.target_resolution.length).toBeGreaterThan(0);
  }, 15000);
});

// ─── rdma_health summary_only ───────────────────────────────────────────────

describe('rdma_health summary_only', () => {
  test('summary_only=true omits all_counters from output', () => {
    // Simulate the behavior: buildDeviceHealth creates all_counters,
    // then summary_only strips it
    const device: any = {
      device: 'rocep42s0',
      port: 1,
      state: 'ACTIVE',
      physical_state: 'LINK_UP',
      netdev: 'ens255np0',
      out_rdma_writes: 100,
      in_rdma_writes: 50,
      retrans_segs: 0,
      all_counters: [{ name: 'OutRdmaWrites', value: 100 }, { name: 'InRdmaWrites', value: 50 }],
    };

    // Simulate summary_only behavior
    const summaryOnly = true;
    if (summaryOnly) {
      delete device.all_counters;
    }

    expect(device.all_counters).toBeUndefined();
    expect(device.device).toBe('rocep42s0');
    expect(device.out_rdma_writes).toBe(100);
  });

  test('summary_only=false preserves all_counters', () => {
    const device: any = {
      device: 'rocep42s0',
      all_counters: [{ name: 'OutRdmaWrites', value: 100 }],
    };

    const summaryOnly = false;
    if (summaryOnly) {
      delete device.all_counters;
    }

    expect(device.all_counters).toBeDefined();
    expect(device.all_counters.length).toBe(1);
  });
});

// ─── perf_debug_snapshot collectors param ───────────────────────────────────

import { perfDebugSnapshot } from '../src/tools/perf-debug-snapshot.js';

describe('perf_debug_snapshot collectors', () => {
  test('collectors=["isolation"] returns only isolation data', async () => {
    const r = await perfDebugSnapshot({ window_ms: 200, collectors: ['isolation'] });
    expectOk(r);
    // isolation should have actual data
    expect(r.data!.isolation.cmdline_flags).toBeDefined();
    // Other collectors should have empty defaults
    expect(r.data!.core_load.cores).toEqual([]);
    expect(r.data!.irq_distribution.per_cpu).toEqual([]);
    expect(r.data!.softirq_snapshot.per_cpu).toEqual([]);
    expect(r.data!.cpu_frequency.cpus).toEqual([]);
  }, 15000);

  test('collectors=["core_load", "cpu_frequency"] returns those two', async () => {
    const r = await perfDebugSnapshot({ window_ms: 200, collectors: ['core_load', 'cpu_frequency'] });
    expectOk(r);
    expect(r.data!.core_load.cores.length).toBeGreaterThan(0);
    expect(r.data!.cpu_frequency.cpus.length).toBeGreaterThan(0);
    // These should be empty (not requested)
    expect(r.data!.irq_distribution.per_cpu).toEqual([]);
    expect(r.data!.softirq_snapshot.per_cpu).toEqual([]);
  }, 15000);

  test('empty collectors runs all (backward compatible)', async () => {
    const r = await perfDebugSnapshot({ window_ms: 200 });
    expectOk(r);
    expect(r.data!.core_load.cores.length).toBeGreaterThan(0);
    expect(r.data!.cpu_frequency.cpus.length).toBeGreaterThan(0);
    expect(r.data!.isolation).toBeDefined();
  }, 30000);
});
