/**
 * Unit tests for /proc/stat parser and CPU utilization computation.
 */
import { describe, test, expect } from '@jest/globals';
import { parseProcTaskStatLine } from '../src/collectors/proc-pid-stat.js';
import { computeCpuDeltas } from '../src/collectors/proc-stat.js';
import { parseCpuList, parseHexMask, cpuListToHexMask } from '../src/utils/proc-reader.js';
import { policyToString, isKernelThread, round2, parseCoreFilter } from '../src/utils/helpers.js';
import type { ProcStatCpuCounters } from '../src/types.js';

describe('parseCpuList', () => {
  test('simple range', () => {
    expect(parseCpuList('0-3')).toEqual([0, 1, 2, 3]);
  });

  test('comma-separated', () => {
    expect(parseCpuList('0,2,4')).toEqual([0, 2, 4]);
  });

  test('mixed', () => {
    expect(parseCpuList('0-2,5,7-9')).toEqual([0, 1, 2, 5, 7, 8, 9]);
  });

  test('single', () => {
    expect(parseCpuList('3')).toEqual([3]);
  });

  test('empty', () => {
    expect(parseCpuList('')).toEqual([]);
  });

  test('whitespace handling', () => {
    expect(parseCpuList(' 0-2 , 5 ')).toEqual([0, 1, 2, 5]);
  });
});

describe('parseHexMask', () => {
  test('simple mask', () => {
    expect(parseHexMask('f')).toEqual([0, 1, 2, 3]);
  });

  test('mask with commas (32-bit grouping)', () => {
    expect(parseHexMask('00000000,0000000f')).toEqual([0, 1, 2, 3]);
  });

  test('single CPU', () => {
    expect(parseHexMask('4')).toEqual([2]); // bit 2 = 0x4
  });

  test('all zeros', () => {
    expect(parseHexMask('0')).toEqual([]);
  });
});

describe('cpuListToHexMask', () => {
  test('simple list', () => {
    expect(cpuListToHexMask([0, 1, 2, 3])).toBe('f');
  });

  test('single CPU', () => {
    expect(cpuListToHexMask([2])).toBe('4');
  });

  test('empty', () => {
    expect(cpuListToHexMask([])).toBe('0');
  });
});

describe('policyToString', () => {
  test('known policies', () => {
    expect(policyToString(0)).toBe('SCHED_OTHER');
    expect(policyToString(1)).toBe('SCHED_FIFO');
    expect(policyToString(2)).toBe('SCHED_RR');
    expect(policyToString(3)).toBe('SCHED_BATCH');
    expect(policyToString(5)).toBe('SCHED_IDLE');
    expect(policyToString(6)).toBe('SCHED_DEADLINE');
  });

  test('unknown policy', () => {
    expect(policyToString(99)).toBe('UNKNOWN');
  });
});

describe('isKernelThread', () => {
  test('kthreadd child (ppid=2)', () => {
    expect(isKernelThread(2, 0, 0)).toBe(true);
  });

  test('PF_KTHREAD flag', () => {
    expect(isKernelThread(1, 0x00200000, 1000)).toBe(true);
  });

  test('normal process', () => {
    expect(isKernelThread(1234, 0, 1000000)).toBe(false);
  });
});

describe('round2', () => {
  test('rounds to 2 decimal places', () => {
    expect(round2(3.14159)).toBe(3.14);
    expect(round2(99.999)).toBe(100);
    expect(round2(0)).toBe(0);
  });
});

describe('parseProcTaskStatLine', () => {
  test('normal process', () => {
    // Realistic /proc/<pid>/stat line: 50 space-separated fields after "(comm) "
    // rest indices: 0=state, 11=utime, 12=stime, 15=priority, 16=nice, 36=processor
    const line = '12345 (test_proc) S 1234 12345 12345 0 -1 4194304 100 0 0 0 50 30 0 0 20 0 1 0 1000 10000000 500 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0';
    const stat = parseProcTaskStatLine(line);
    expect(stat).not.toBeNull();
    expect(stat!.pid).toBe(12345);
    expect(stat!.comm).toBe('test_proc');
    expect(stat!.state).toBe('S');
    expect(stat!.utime).toBe(50);
    expect(stat!.stime).toBe(30);
    expect(stat!.nice).toBe(0);
    expect(stat!.processor).toBe(3);
  });

  test('comm with spaces', () => {
    // processor at rest[36]=5
    const line = '67890 (my process) R 1 67890 67890 0 -1 0 0 0 0 0 100 200 0 0 20 0 1 0 500 20000000 1000 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 0 5 0 0 0 0 0 0 0 0 0 0 0 0 0';
    const stat = parseProcTaskStatLine(line);
    expect(stat).not.toBeNull();
    expect(stat!.pid).toBe(67890);
    expect(stat!.comm).toBe('my process');
    expect(stat!.state).toBe('R');
    expect(stat!.processor).toBe(5);
  });

  test('comm with closing paren', () => {
    // processor at rest[36]=7
    const line = '111 (a)b) S 1 111 111 0 -1 0 0 0 0 0 10 20 0 0 20 0 1 0 300 5000000 200 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 0 7 0 0 0 0 0 0 0 0 0 0 0 0 0';
    const stat = parseProcTaskStatLine(line);
    expect(stat).not.toBeNull();
    expect(stat!.pid).toBe(111);
    expect(stat!.comm).toBe('a)b');
  });
});

describe('computeCpuDeltas', () => {
  test('basic utilization computation', () => {
    const before: ProcStatCpuCounters[] = [
      { cpu: 0, user: 100, nice: 0, system: 50, idle: 800, iowait: 10, irq: 5, softirq: 5, steal: 0, guest: 0, guest_nice: 0 },
    ];
    const after: ProcStatCpuCounters[] = [
      { cpu: 0, user: 200, nice: 0, system: 100, idle: 1600, iowait: 20, irq: 10, softirq: 10, steal: 0, guest: 0, guest_nice: 0 },
    ];

    const deltas = computeCpuDeltas(before, after);
    expect(deltas).toHaveLength(1);
    expect(deltas[0].cpu).toBe(0);
    // Total = 100+50+800+10+5+5 = 970
    // Active = 970-800-10 = 160
    expect(deltas[0].util_pct).toBeGreaterThan(0);
    expect(deltas[0].idle_pct).toBeGreaterThan(0);
    expect(deltas[0].util_pct + deltas[0].idle_pct + deltas[0].iowait_pct).toBeCloseTo(100, 0);
  });

  test('fully idle CPU', () => {
    const before: ProcStatCpuCounters[] = [
      { cpu: 0, user: 0, nice: 0, system: 0, idle: 1000, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
    ];
    const after: ProcStatCpuCounters[] = [
      { cpu: 0, user: 0, nice: 0, system: 0, idle: 2000, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
    ];

    const deltas = computeCpuDeltas(before, after);
    expect(deltas[0].util_pct).toBe(0);
    expect(deltas[0].idle_pct).toBe(100);
  });

  test('fully busy CPU', () => {
    const before: ProcStatCpuCounters[] = [
      { cpu: 0, user: 0, nice: 0, system: 0, idle: 0, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
    ];
    const after: ProcStatCpuCounters[] = [
      { cpu: 0, user: 1000, nice: 0, system: 0, idle: 0, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
    ];

    const deltas = computeCpuDeltas(before, after);
    expect(deltas[0].util_pct).toBe(100);
    expect(deltas[0].idle_pct).toBe(0);
  });

  test('deterministic ordering by CPU ID', () => {
    const before: ProcStatCpuCounters[] = [
      { cpu: 2, user: 0, nice: 0, system: 0, idle: 100, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
      { cpu: 0, user: 0, nice: 0, system: 0, idle: 100, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
      { cpu: 1, user: 0, nice: 0, system: 0, idle: 100, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
    ];
    const after: ProcStatCpuCounters[] = [
      { cpu: 2, user: 50, nice: 0, system: 0, idle: 200, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
      { cpu: 0, user: 30, nice: 0, system: 0, idle: 200, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
      { cpu: 1, user: 10, nice: 0, system: 0, idle: 200, iowait: 0, irq: 0, softirq: 0, steal: 0, guest: 0, guest_nice: 0 },
    ];

    const deltas = computeCpuDeltas(before, after);
    expect(deltas[0].cpu).toBe(0);
    expect(deltas[1].cpu).toBe(1);
    expect(deltas[2].cpu).toBe(2);
  });
});

// ─── parseCoreFilter ────────────────────────────────────────────────────────

describe('parseCoreFilter', () => {
  test('null input returns null', () => {
    expect(parseCoreFilter(null)).toBeNull();
    expect(parseCoreFilter(undefined)).toBeNull();
    expect(parseCoreFilter("")).toBeNull();
    expect(parseCoreFilter("  ")).toBeNull();
  });

  test('single number', () => {
    const result = parseCoreFilter("4");
    expect(result).toEqual(new Set([4]));
  });

  test('comma-separated list', () => {
    const result = parseCoreFilter("4,5,6,7");
    expect(result).toEqual(new Set([4, 5, 6, 7]));
  });

  test('range', () => {
    const result = parseCoreFilter("4-8");
    expect(result).toEqual(new Set([4, 5, 6, 7, 8]));
  });

  test('mixed ranges and singles', () => {
    const result = parseCoreFilter("4-6,10,20-22");
    expect(result).toEqual(new Set([4, 5, 6, 10, 20, 21, 22]));
  });

  test('10 isolated cores', () => {
    const result = parseCoreFilter("4-13");
    expect(result!.size).toBe(10);
    expect(result!.has(4)).toBe(true);
    expect(result!.has(13)).toBe(true);
    expect(result!.has(3)).toBe(false);
    expect(result!.has(14)).toBe(false);
  });

  test('spaces in input are handled', () => {
    const result = parseCoreFilter(" 4 , 5 , 6-8 ");
    expect(result).toEqual(new Set([4, 5, 6, 7, 8]));
  });
});
