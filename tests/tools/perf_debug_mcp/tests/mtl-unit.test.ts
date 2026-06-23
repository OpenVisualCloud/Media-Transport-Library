/**
 * Unit tests for MTL tool parsers and utilities.
 *
 * Tests the pure parsing logic extracted from MTL tools without
 * requiring SSH connections or remote hosts.
 */
import { describe, test, expect } from "@jest/globals";
import { parseMtlStatBlock } from "../src/tools/mtl-session-stats.js";
import { flattenObject } from "../src/tools/mtl-live-stats.js";
import type { MtlSessionStatsData, MtlAggregateStats, MtlSessionAggregate } from "../src/types-mtl.js";

// ─── parseMtlStatBlock — MTL stat dump parser ────────────────────────────

describe("parseMtlStatBlock", () => {
  test("parses DEV rate line", () => {
    const lines = [
      "MTL: 2024-06-15 12:34:56, DEV(0): Avr rate, tx: 4971.23 Mb/s, rx: 4980.45 Mb/s, pkts, tx: 1234567, rx: 2345678",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.timestamp).toBe("2024-06-15 12:34:56");
    expect(result.dev_stats).toHaveLength(1);
    expect(result.dev_stats[0]).toEqual({
      port_index: 0,
      tx_rate_mbps: 4971.23,
      rx_rate_mbps: 4980.45,
      tx_pkts: 1234567,
      rx_pkts: 2345678,
    });
  });

  test("parses multiple DEV ports", () => {
    const lines = [
      "MTL: 2024-06-15 12:34:56, DEV(0): Avr rate, tx: 100.00 Mb/s, rx: 200.00 Mb/s, pkts, tx: 100, rx: 200",
      "DEV(1): Avr rate, tx: 300.00 Mb/s, rx: 400.00 Mb/s, pkts, tx: 300, rx: 400",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.dev_stats).toHaveLength(2);
    expect(result.dev_stats[1].port_index).toBe(1);
    expect(result.dev_stats[1].tx_rate_mbps).toBe(300.0);
  });

  test("parses SCH scheduler line", () => {
    const lines = [
      "SCH(0:sch_0): tasklets 2, lcore 30(t_pid: 12345), avg loop 5432 ns",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.sch_stats).toHaveLength(1);
    expect(result.sch_stats[0]).toEqual({
      sch_index: 0,
      name: "sch_0",
      tasklets: 2,
      lcore: 30,
      thread_pid: 12345,
      avg_loop_ns: 5432,
    });
  });

  test("parses CNI line", () => {
    const lines = [
      "CNI(0): eth_rx_rate 0.12 Mb/s, eth_rx_cnt 567",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.cni_stats).toHaveLength(1);
    expect(result.cni_stats[0]).toEqual({
      port_index: 0,
      eth_rx_rate_mbps: 0.12,
      eth_rx_cnt: 567,
    });
  });

  test("parses PTP line", () => {
    const lines = [
      "PTP(0): time 1718451296000000000, 2024-06-15 12:34:56",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.ptp_stats).toHaveLength(1);
    expect(result.ptp_stats[0]).toEqual({
      port_index: 0,
      time_ns: 1718451296000000000,
      time_human: "2024-06-15 12:34:56",
    });
  });

  test("parses RX_VIDEO_SESSION with throughput and burst", () => {
    const lines = [
      "RX_VIDEO_SESSION(0,0:rx_s0): fps 59.94 frames 600 pkts 150000 redundant 1500",
      "RX_VIDEO_SESSION(0,0): throughput 4971.23 Mb/s, cpu busy 0.85",
      "RX_VIDEO_SESSION(0,0): succ burst max 128, avg 64.5",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions).toHaveLength(1);
    const s = result.video_sessions[0];
    expect(s.sch_index).toBe(0);
    expect(s.session_index).toBe(0);
    expect(s.name).toBe("rx_s0");
    expect(s.fps).toBeCloseTo(59.94);
    expect(s.frames).toBe(600);
    expect(s.pkts).toBe(150000);
    expect(s.redundant_pkts).toBe(1500);
    expect(s.throughput_mbps).toBeCloseTo(4971.23);
    expect(s.cpu_busy).toBeCloseTo(0.85);
    expect(s.burst_max).toBe(128);
    expect(s.burst_avg).toBeCloseTo(64.5);
  });

  test("parses TX_VIDEO_SESSION", () => {
    const lines = [
      "TX_VIDEO_SESSION(0,0:tx_s0): fps 60.00 frames 600 pkts 120000",
      "TX_VIDEO_SESSION(0,0): throughput 5000.00 Mb/s, cpu busy 0.42",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions).toHaveLength(1);
    const s = result.video_sessions[0];
    expect(s.name).toBe("tx_s0");
    expect(s.fps).toBeCloseTo(60.0);
    expect(s.throughput_mbps).toBeCloseTo(5000.0);
    expect(s.cpu_busy).toBeCloseTo(0.42);
  });

  test("parses full stat block", () => {
    const lines = [
      "MTL: 2024-06-15 12:34:56, DEV(0): Avr rate, tx: 5000.00 Mb/s, rx: 5000.00 Mb/s, pkts, tx: 100000, rx: 200000",
      "SCH(0:sch_rx): tasklets 4, lcore 30(t_pid: 9876), avg loop 3000 ns",
      "SCH(1:sch_tx): tasklets 2, lcore 31(t_pid: 9877), avg loop 2000 ns",
      "CNI(0): eth_rx_rate 0.50 Mb/s, eth_rx_cnt 1000",
      "PTP(0): time 1718451296000000000, 2024-06-15 12:34:56",
      "RX_VIDEO_SESSION(0,0:stream0): fps 59.94 frames 600 pkts 150000",
      "RX_VIDEO_SESSION(0,0): throughput 4900.00 Mb/s, cpu busy 0.75",
      "RX_VIDEO_SESSION(0,0): succ burst max 256, avg 128.0",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.timestamp).toBe("2024-06-15 12:34:56");
    expect(result.dev_stats).toHaveLength(1);
    expect(result.sch_stats).toHaveLength(2);
    expect(result.cni_stats).toHaveLength(1);
    expect(result.ptp_stats).toHaveLength(1);
    expect(result.video_sessions).toHaveLength(1);
    expect(result.video_sessions[0].burst_max).toBe(256);
  });

  test("handles empty block", () => {
    const result = parseMtlStatBlock([]);
    expect(result.timestamp).toBe("");
    expect(result.dev_stats).toHaveLength(0);
    expect(result.sch_stats).toHaveLength(0);
    expect(result.cni_stats).toHaveLength(0);
    expect(result.ptp_stats).toHaveLength(0);
    expect(result.video_sessions).toHaveLength(0);
  });

  test("handles unrelated log lines gracefully", () => {
    const lines = [
      "MTL: 2024-06-15 12:34:56, some random log line",
      "another line that doesn't match anything",
      "DEBUG: memory allocated",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.timestamp).toBe("2024-06-15 12:34:56");
    expect(result.dev_stats).toHaveLength(0);
  });
});

// ─── flattenObject — nested JSON flattener ───────────────────────────────

describe("flattenObject", () => {
  test("flattens simple nested object", () => {
    const obj = { a: { b: 1 }, c: 2 };
    expect(flattenObject(obj)).toEqual({ "a.b": 1, c: 2 });
  });

  test("flattens deeply nested object", () => {
    const obj = { a: { b: { c: { d: "deep" } } } };
    expect(flattenObject(obj)).toEqual({ "a.b.c.d": "deep" });
  });

  test("handles flat object", () => {
    const obj = { x: 1, y: "two", z: 3 };
    expect(flattenObject(obj)).toEqual({ x: 1, y: "two", z: 3 });
  });

  test("handles empty object", () => {
    expect(flattenObject({})).toEqual({});
  });

  test("converts booleans to strings", () => {
    const obj = { enabled: true, disabled: false };
    expect(flattenObject(obj)).toEqual({ enabled: "true", disabled: "false" });
  });

  test("skips arrays", () => {
    const obj = { list: [1, 2, 3], name: "test" };
    expect(flattenObject(obj)).toEqual({ name: "test" });
  });

  test("preserves numeric types", () => {
    const obj = { fps: 59.94, frames: 600 };
    const result = flattenObject(obj);
    expect(typeof result.fps).toBe("number");
    expect(result.fps).toBeCloseTo(59.94);
  });

  test("handles mixed nested types", () => {
    const obj = {
      stats: {
        rx: { bytes: 1000, errors: 0 },
        tx: { bytes: 2000, errors: 1 },
      },
      name: "eth0",
    };
    expect(flattenObject(obj)).toEqual({
      "stats.rx.bytes": 1000,
      "stats.rx.errors": 0,
      "stats.tx.bytes": 2000,
      "stats.tx.errors": 1,
      name: "eth0",
    });
  });

  test("handles null values gracefully", () => {
    // null is typeof 'object' but should be filtered (not Array and not null)
    const obj = { a: null as unknown, b: 5 };
    // null is not number, string, or boolean, so it's skipped
    expect(flattenObject(obj as Record<string, unknown>)).toEqual({ b: 5 });
  });
});

// ─── SSH exec utility — basic structure tests ────────────────────────────

describe("ssh-exec utility", () => {
  test("module exports expected functions", async () => {
    const mod = await import("../src/utils/ssh-exec.js");
    expect(typeof mod.sshExec).toBe("function");
    expect(typeof mod.sshExecOrThrow).toBe("function");
    expect(typeof mod.sshExecSafe).toBe("function");
  });
});

// ─── MTL types — verify type exports ────────────────────────────────────

describe("MTL types module", () => {
  test("module is importable", async () => {
    const mod = await import("../src/types-mtl.js");
    // Types are compile-time only, but module should import cleanly
    expect(mod).toBeDefined();
  });
});

// ─── Capabilities list includes MTL tools ────────────────────────────────

describe("capabilities includes MTL tools", () => {
  test("mtl tools appear in capabilities response", async () => {
    const { capabilities } = await import("../src/tools/capabilities.js");
    const result = await capabilities();
    expect(result.data).toBeDefined();
    const tools = result.data!.available_tools;
    const mtlTools = [
      "mtl_manager_status",
      "mtl_dpdk_telemetry",
      "mtl_instance_processes",
      "mtl_session_stats",
      "mtl_app_latency",
      "mtl_live_stats",
      "mtl_nic_pf_stats",
      "mtl_hugepage_usage",
      "mtl_lcore_shm",
      "mtl_influxdb_query",
    ];
    for (const tool of mtlTools) {
      expect(tools).toContain(tool);
    }
  });
});

// ─── mtl_session_stats output types ─────────────────────────────────────────

describe("MtlSessionStatsData types", () => {
  test("steady_state_dumps field exists on type", () => {
    const data: MtlSessionStatsData = {
      log_file: "/dev/shm/test.log",
      latest_dump: null,
      dumps_found: 10,
      steady_state_dumps: 5,
    };
    expect(data.steady_state_dumps).toBe(5);
    expect(data.dumps_found).toBe(10);
  });

  test("MtlAggregateStats expresses mean/stddev/min/max", () => {
    const stats: MtlAggregateStats = { mean: 59.5, stddev: 0.5, min: 59.0, max: 60.0 };
    expect(stats.mean).toBe(59.5);
    expect(stats.stddev).toBe(0.5);
  });

  test("MtlSessionAggregate wraps per-session stats", () => {
    const agg: MtlSessionAggregate = {
      session: "video_0(0,0)",
      sample_count: 5,
      fps: { mean: 59.8, stddev: 0.2, min: 59.5, max: 60.1 },
      throughput_mbps: { mean: 4500, stddev: 10, min: 4480, max: 4520 },
    };
    expect(agg.session).toBe("video_0(0,0)");
    expect(agg.fps.mean).toBeCloseTo(59.8);
    expect(agg.throughput_mbps!.mean).toBe(4500);
  });

  test("fps_mean field available on MtlSessionStatsData", () => {
    const data: MtlSessionStatsData = {
      log_file: "/dev/shm/test.log",
      latest_dump: null,
      dumps_found: 5,
      steady_state_dumps: 5,
      fps_mean: 59.8,
      fps_min: 59.5,
      fps_max: 60.1,
      fps_stddev: 0.2,
    };
    expect(data.fps_mean).toBeCloseTo(59.8);
  });
});

// ─── parseMtlStatBlock: verify multi-dump last-N slicing logic ──────────────

describe("parseMtlStatBlock multi-dump extraction", () => {
  test("extracts multiple dumps from log output", () => {
    // Simulate 3 stat dump blocks
    const dumps = [];
    for (let i = 0; i < 3; i++) {
      const fps = 48 + i * 6; // 48, 54, 60
      const lines = [
        `MTL: 2025-01-15 10:00:${String(10 * i).padStart(2, "0")} blah`,
        `RX_VIDEO_SESSION(0,0:video_0): fps ${fps} frames 100 pkts 200`,
        `RX_VIDEO_SESSION(0,0): throughput 4500.0 Mb/s, cpu busy 30.5`,
        `RX_VIDEO_SESSION(0,0): succ burst max 10, avg 5.0`,
      ];
      dumps.push(parseMtlStatBlock(lines));
    }

    // Last dump should have fps=60 (steady state)
    expect(dumps[dumps.length - 1].video_sessions[0].fps).toBe(60);
    // First dump has startup fps=48
    expect(dumps[0].video_sessions[0].fps).toBe(48);

    // Simulating last_dumps=2: slice the last 2
    const steadyState = dumps.slice(-2);
    expect(steadyState.length).toBe(2);
    expect(steadyState[0].video_sessions[0].fps).toBe(54);
    expect(steadyState[1].video_sessions[0].fps).toBe(60);

    // Mean of last 2 should be 57, not dragged down by 48
    const fpsValues = steadyState.map(d => d.video_sessions[0].fps);
    const mean = fpsValues.reduce((s, v) => s + v, 0) / fpsValues.length;
    expect(mean).toBe(57);
  });
});

// ─── Bug Fix #1: DEV error counters ─────────────────────────────────────────

describe("Bug #1: DEV error counters (rx_hw_dropped, rx_err, rx_nombuf, tx_err)", () => {
  test("parses DEV Status line after Avr rate line", () => {
    // Real MTL format: err("DEV(%d): Status: rx_hw_dropped_packets %" PRIu64 ...
    const lines = [
      "MTL: 2026-03-04 09:51:28, DEV(0): Avr rate, tx: 0.000000 Mb/s, rx: 2604.174342 Mb/s, pkts, tx: 0, rx: 2467419",
      "DEV(0): Status: rx_hw_dropped_packets 12345 rx_err_packets 67 rx_nombuf_packets 890 tx_err_packets 12",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.dev_stats).toHaveLength(1);
    const dev = result.dev_stats[0];
    expect(dev.rx_hw_dropped).toBe(12345);
    expect(dev.rx_err).toBe(67);
    expect(dev.rx_nombuf).toBe(890);
    expect(dev.tx_err).toBe(12);
  });

  test("DEV Status line with Error: prefix (err() log level)", () => {
    const lines = [
      "MTL: 2026-03-04 09:51:28, DEV(0): Avr rate, tx: 100.00 Mb/s, rx: 200.00 Mb/s, pkts, tx: 100, rx: 200",
      "Error: DEV(0): Status: rx_hw_dropped_packets 999 rx_err_packets 0 rx_nombuf_packets 0 tx_err_packets 0",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.dev_stats[0].rx_hw_dropped).toBe(999);
    expect(result.dev_stats[0].rx_err).toBe(0);
  });

  test("DEV Status not present leaves error fields undefined", () => {
    const lines = [
      "MTL: 2026-03-04 09:51:49, DEV(0): Avr rate, tx: 2612.952115 Mb/s, rx: 0.000000 Mb/s, pkts, tx: 2468253, rx: 0",
      "MTL: 2026-03-04 09:51:49, SCH(0:sch_0): tasklets 2, lcore 1(t_pid: 619335), avg loop 211 ns",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.dev_stats[0].rx_hw_dropped).toBeUndefined();
    expect(result.dev_stats[0].rx_err).toBeUndefined();
    expect(result.dev_stats[0].rx_nombuf).toBeUndefined();
    expect(result.dev_stats[0].tx_err).toBeUndefined();
  });

  test("DEV Status matched to correct port index", () => {
    const lines = [
      "MTL: 2026-03-04 09:51:28, DEV(0): Avr rate, tx: 0.00 Mb/s, rx: 2604.17 Mb/s, pkts, tx: 0, rx: 2467419",
      "MTL: 2026-03-04 09:51:28, DEV(1): Avr rate, tx: 0.00 Mb/s, rx: 2603.83 Mb/s, pkts, tx: 0, rx: 2467104",
      "DEV(1): Status: rx_hw_dropped_packets 42 rx_err_packets 0 rx_nombuf_packets 0 tx_err_packets 0",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.dev_stats[0].rx_hw_dropped).toBeUndefined(); // DEV(0) — no errors
    expect(result.dev_stats[1].rx_hw_dropped).toBe(42);       // DEV(1) — has errors
  });
});

// ─── Bug Fix #2: RX quality metrics (incomplete, idx_error, offset_error, etc.) ─

describe("Bug #2: RX quality metrics", () => {
  test("parses incomplete frames and pkt error breakdown (real format)", () => {
    // Exact format from live poc sender.log
    const lines = [
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0:mtl_mxl_poc_rx): fps 59.902375 frames 599 pkts 2468880 redundant -1250275391",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): throughput 5227.151514 Mb/s, cpu busy 1.740302",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): incomplete frames 1, pkts (idx error: 0, offset error: 0, idx out of bitmap: 0, missed: 18)",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): out of order pkts 34",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): succ burst max 94, avg 1.213120",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): tasklet time avg 0.08us max 87.43us min 0.03us",
    ];
    const result = parseMtlStatBlock(lines);
    const s = result.video_sessions[0];
    expect(s.incomplete_frames).toBe(1);
    expect(s.idx_error).toBe(0);
    expect(s.offset_error).toBe(0);
    expect(s.idx_out_of_bitmap).toBe(0);
    expect(s.missed_pkts).toBe(18);
  });

  test("quality metrics absent when no issues", () => {
    const lines = [
      "RX_VIDEO_SESSION(0,0:rx16_0): fps 60.018592 frames 600 pkts 2468202",
      "RX_VIDEO_SESSION(0,0): throughput 2612.898120 Mb/s, cpu busy 24.784302",
      "RX_VIDEO_SESSION(0,0): succ burst max 128, avg 3.121161",
      "RX_VIDEO_SESSION(0,0): tasklet time avg 0.34us max 169.46us min 0.02us",
    ];
    const result = parseMtlStatBlock(lines);
    const s = result.video_sessions[0];
    expect(s.incomplete_frames).toBeUndefined();
    expect(s.idx_error).toBeUndefined();
    expect(s.missed_pkts).toBeUndefined();
  });
});

// ─── Bug Fix #3: out_of_order counter ────────────────────────────────────────

describe("Bug #3: out-of-order counter", () => {
  test("parses out of order pkts (real format)", () => {
    const lines = [
      "RX_VIDEO_SESSION(0,0:mtl_mxl_poc_rx): fps 59.902375 frames 599 pkts 2468880",
      "RX_VIDEO_SESSION(0,0): throughput 5227.15 Mb/s, cpu busy 1.74",
      "RX_VIDEO_SESSION(0,0): out of order pkts 34",
      "RX_VIDEO_SESSION(0,0): succ burst max 94, avg 1.21",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions[0].out_of_order_pkts).toBe(34);
  });

  test("out_of_order absent when zero/not reported", () => {
    const lines = [
      "RX_VIDEO_SESSION(0,0:rx16_0): fps 60.00 frames 600 pkts 2468200",
      "RX_VIDEO_SESSION(0,0): throughput 2612.89 Mb/s, cpu busy 24.78",
      "RX_VIDEO_SESSION(0,0): succ burst max 128, avg 3.12",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions[0].out_of_order_pkts).toBeUndefined();
  });
});

// ─── Bug Fix #4: Burst lookahead too short ───────────────────────────────────

describe("Bug #4: Wider lookahead for burst/quality/tasklet lines", () => {
  test("parses burst at offset 5+ (old 2-line lookahead would miss)", () => {
    // Real scenario: incomplete + out_of_order push burst to line i+5
    const lines = [
      "RX_VIDEO_SESSION(0,0:mtl_mxl_poc_rx): fps 59.90 frames 599 pkts 2468880 redundant 99",
      "RX_VIDEO_SESSION(0,0): throughput 5227.15 Mb/s, cpu busy 1.74",
      "RX_VIDEO_SESSION(0,0): incomplete frames 1, pkts (idx error: 0, offset error: 0, idx out of bitmap: 0, missed: 18)",
      "RX_VIDEO_SESSION(0,0): out of order pkts 34",
      "RX_VIDEO_SESSION(0,0): succ burst max 94, avg 1.213120",
      "RX_VIDEO_SESSION(0,0): tasklet time avg 0.08us max 87.43us min 0.03us",
      "RX_VIDEO_SESSION(0,0): burst avg 0.28us, handler avg 0.10us",
    ];
    const result = parseMtlStatBlock(lines);
    const s = result.video_sessions[0];
    // All fields should be captured despite being at offset 4-6 from session header
    expect(s.throughput_mbps).toBeCloseTo(5227.15);
    expect(s.incomplete_frames).toBe(1);
    expect(s.out_of_order_pkts).toBe(34);
    expect(s.burst_max).toBe(94);
    expect(s.burst_avg).toBeCloseTo(1.21312);
    expect(s.tasklet_time_avg_us).toBeCloseTo(0.08);
  });

  test("multi-session: each session gets its own continuation lines", () => {
    // Real poc_16 format: session 0 lines then session 1 lines
    const lines = [
      "RX_VIDEO_SESSION(0,0:rx16_0): fps 60.018592 frames 600 pkts 2468202",
      "RX_VIDEO_SESSION(0,0): throughput 2612.898120 Mb/s, cpu busy 24.784302",
      "RX_VIDEO_SESSION(0,0): succ burst max 128, avg 3.121161",
      "RX_VIDEO_SESSION(0,0): tasklet time avg 0.34us max 169.46us min 0.02us",
      "RX_VIDEO_SESSION(0,0): burst avg 0.14us, handler avg 1.03us",
      "RX_VIDEO_SESSION(0,1:rx16_1): fps 60.018600 frames 600 pkts 2468203",
      "RX_VIDEO_SESSION(0,1): throughput 2612.899173 Mb/s, cpu busy 24.784302",
      "RX_VIDEO_SESSION(0,1): succ burst max 128, avg 3.117020",
      "RX_VIDEO_SESSION(0,1): tasklet time avg 0.34us max 212.40us min 0.02us",
      "RX_VIDEO_SESSION(0,1): burst avg 0.13us, handler avg 1.03us",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions).toHaveLength(2);
    // Session 0
    expect(result.video_sessions[0].name).toBe("rx16_0");
    expect(result.video_sessions[0].burst_max).toBe(128);
    expect(result.video_sessions[0].tasklet_time_avg_us).toBeCloseTo(0.34);
    expect(result.video_sessions[0].tasklet_time_max_us).toBeCloseTo(169.46);
    // Session 1
    expect(result.video_sessions[1].name).toBe("rx16_1");
    expect(result.video_sessions[1].burst_max).toBe(128);
    expect(result.video_sessions[1].tasklet_time_max_us).toBeCloseTo(212.40);
  });
});

// ─── Bug Fix #5: TX throughput regex ─────────────────────────────────────────

describe("Bug #5: TX throughput dual-value format", () => {
  test("parses TX throughput with dual Mb/s values (real format)", () => {
    // Real synthtx.log: "throughput X Mb/s: Y Mb/s, cpu busy Z"
    const lines = [
      "TX_VIDEO_SESSION(0,0:synth_tx): fps 59.999709 frames 600 pkts 2469012:2469012 inflight 617323:617403",
      "TX_VIDEO_SESSION(0,0): throughput 2613.743063 Mb/s: 2613.743063 Mb/s, cpu busy 1.350234",
    ];
    const result = parseMtlStatBlock(lines);
    const s = result.video_sessions[0];
    expect(s.throughput_mbps).toBeCloseTo(2613.743063);
    expect(s.cpu_busy).toBeCloseTo(1.350234);
  });

  test("handles TX throughput with single Mb/s value (future-proof)", () => {
    const lines = [
      "TX_VIDEO_SESSION(0,0:tx_s0): fps 60.00 frames 600 pkts 120000",
      "TX_VIDEO_SESSION(0,0): throughput 5000.00 Mb/s, cpu busy 0.42",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions[0].throughput_mbps).toBeCloseTo(5000.0);
    expect(result.video_sessions[0].cpu_busy).toBeCloseTo(0.42);
  });
});

// ─── Bug Fix #6: Tasklet time (TX and RX) ────────────────────────────────────

describe("Bug #6: Tasklet time avg/max/min", () => {
  test("parses RX tasklet time (real format)", () => {
    const lines = [
      "RX_VIDEO_SESSION(0,0:mtl_mxl_poc_rx): fps 59.90 frames 599 pkts 2468880",
      "RX_VIDEO_SESSION(0,0): throughput 5227.15 Mb/s, cpu busy 1.74",
      "RX_VIDEO_SESSION(0,0): tasklet time avg 0.08us max 87.43us min 0.03us",
    ];
    const result = parseMtlStatBlock(lines);
    const s = result.video_sessions[0];
    expect(s.tasklet_time_avg_us).toBeCloseTo(0.08);
    expect(s.tasklet_time_max_us).toBeCloseTo(87.43);
    expect(s.tasklet_time_min_us).toBeCloseTo(0.03);
  });

  test("parses TX tasklet time (real format, same pattern)", () => {
    const lines = [
      "TX_VIDEO_SESSION(0,0:synth_tx): fps 59.999709 frames 600 pkts 2469012:2469012 inflight 617323:617403",
      "TX_VIDEO_SESSION(0,0): throughput 2613.74 Mb/s: 2613.74 Mb/s, cpu busy 1.35",
      "TX_VIDEO_SESSION(0,0): 2 frames are in trans, total 2",
      "TX_VIDEO_SESSION(0,0): tasklet time avg 0.01us max 283.11us min 0.01us",
    ];
    const result = parseMtlStatBlock(lines);
    const s = result.video_sessions[0];
    expect(s.tasklet_time_avg_us).toBeCloseTo(0.01);
    expect(s.tasklet_time_max_us).toBeCloseTo(283.11);
    expect(s.tasklet_time_min_us).toBeCloseTo(0.01);
  });

  test("tasklet time absent when not reported", () => {
    const lines = [
      "RX_VIDEO_SESSION(0,0:rx_s0): fps 59.94 frames 600 pkts 150000",
      "RX_VIDEO_SESSION(0,0): throughput 4971.23 Mb/s, cpu busy 0.85",
      "RX_VIDEO_SESSION(0,0): succ burst max 128, avg 64.5",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.video_sessions[0].tasklet_time_avg_us).toBeUndefined();
  });
});

// ─── Bug Fix #7: SCH extended stats (time + per-tasklet timing) ──────────────

describe("Bug #7: SCH extended stats", () => {
  test("parses SCH time avg/max/min (real format)", () => {
    const lines = [
      "MTL: 2026-03-04 09:55:14, SCH(0:sch_0): tasklets 1, lcore 12(t_pid: 617445), avg loop 2845 ns",
      "MTL: 2026-03-04 09:55:14, SCH(0): tasklet_calls 3324886, empty_loops 1675642",
      "MTL: 2026-03-04 09:55:14, SCH(0): time avg 3.00us max 1168.01us min 0.44us",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.sch_stats).toHaveLength(1);
    const sch = result.sch_stats[0];
    expect(sch.time_avg_us).toBeCloseTo(3.0);
    expect(sch.time_max_us).toBeCloseTo(1168.01);
    expect(sch.time_min_us).toBeCloseTo(0.44);
  });

  test("parses per-tasklet timing details (real format)", () => {
    const lines = [
      "SCH(0:sch_0): tasklets 2, lcore 1(t_pid: 619335), avg loop 211 ns",
      "SCH(0): tasklet_calls 94780596, empty_loops 1502096",
      "SCH(0): time avg 0.20us max 284.02us min 0.10us",
      "SCH(0,0): tasklet tx_video_sessions_mgr, avg 0.08us max 283.19us min 0.03us",
      "SCH(0,1): tasklet video_transmitter, avg 0.07us max 67.24us min 0.03us",
    ];
    const result = parseMtlStatBlock(lines);
    const sch = result.sch_stats[0];
    expect(sch.tasklet_details).toBeDefined();
    expect(sch.tasklet_details).toHaveLength(2);
    // Tasklet 0
    expect(sch.tasklet_details![0].tasklet_index).toBe(0);
    expect(sch.tasklet_details![0].name).toBe("tx_video_sessions_mgr");
    expect(sch.tasklet_details![0].avg_us).toBeCloseTo(0.08);
    expect(sch.tasklet_details![0].max_us).toBeCloseTo(283.19);
    expect(sch.tasklet_details![0].min_us).toBeCloseTo(0.03);
    // Tasklet 1
    expect(sch.tasklet_details![1].tasklet_index).toBe(1);
    expect(sch.tasklet_details![1].name).toBe("video_transmitter");
    expect(sch.tasklet_details![1].avg_us).toBeCloseTo(0.07);
  });

  test("SCH extended stats absent when not reported (basic SCH only)", () => {
    const lines = [
      "SCH(0:sch_0): tasklets 2, lcore 30(t_pid: 12345), avg loop 5432 ns",
      "CNI(0): eth_rx_rate 0.12 Mb/s, eth_rx_cnt 567",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.sch_stats[0].time_avg_us).toBeUndefined();
    expect(result.sch_stats[0].tasklet_details).toBeUndefined();
  });

  test("multiple SCH blocks each get their own tasklet details", () => {
    // Real poc_16 sender format: 2 schedulers each with 1 tasklet
    const lines = [
      "SCH(0:sch_0): tasklets 1, lcore 12(t_pid: 617445), avg loop 2845 ns",
      "SCH(0): tasklet_calls 3324886, empty_loops 1675642",
      "SCH(0): time avg 3.00us max 1168.01us min 0.44us",
      "SCH(0,0): tasklet rvs_pkt_rx, avg 2.95us max 1165.98us min 0.40us",
      "SCH(1:sch_1): tasklets 1, lcore 13(t_pid: 617446), avg loop 2814 ns",
      "SCH(1): tasklet_calls 3363502, empty_loops 1851490",
      "SCH(1): time avg 2.96us max 1481.99us min 0.44us",
      "SCH(1,0): tasklet rvs_pkt_rx, avg 2.91us max 1481.69us min 0.40us",
    ];
    const result = parseMtlStatBlock(lines);
    expect(result.sch_stats).toHaveLength(2);
    // SCH 0
    expect(result.sch_stats[0].time_avg_us).toBeCloseTo(3.0);
    expect(result.sch_stats[0].tasklet_details).toHaveLength(1);
    expect(result.sch_stats[0].tasklet_details![0].name).toBe("rvs_pkt_rx");
    expect(result.sch_stats[0].tasklet_details![0].max_us).toBeCloseTo(1165.98);
    // SCH 1
    expect(result.sch_stats[1].time_avg_us).toBeCloseTo(2.96);
    expect(result.sch_stats[1].tasklet_details).toHaveLength(1);
    expect(result.sch_stats[1].tasklet_details![0].max_us).toBeCloseTo(1481.69);
  });
});

// ─── Full integration: Real full stat dump from live system ──────────────────

describe("Full stat dump integration (real .52 log output)", () => {
  test("parses complete poc sender stat dump (RX, redundancy, errors)", () => {
    // Exact lines from /dev/shm/poc_logs/sender.log on .52
    const lines = [
      "MTL: 2026-03-04 09:51:28, DEV(0): Avr rate, tx: 0.000000 Mb/s, rx: 2604.174342 Mb/s, pkts, tx: 0, rx: 2467419",
      "MTL: 2026-03-04 09:51:28, DEV(1): Avr rate, tx: 0.000000 Mb/s, rx: 2603.837577 Mb/s, pkts, tx: 0, rx: 2467104",
      "MTL: 2026-03-04 09:51:28, SCH(0:sch_0): tasklets 1, lcore 3(t_pid: 616970), avg loop 203 ns",
      "MTL: 2026-03-04 09:51:28, SCH(0): tasklet_calls 49761902, empty_loops 46327268",
      "MTL: 2026-03-04 09:51:28, SCH(0): time avg 0.19us max 90.28us min 0.06us",
      "MTL: 2026-03-04 09:51:28, SCH(0,0): tasklet rvs_pkt_rx, avg 0.15us max 90.23us min 0.03us",
      "MTL: 2026-03-04 09:51:28, CNI(0): eth_rx_rate 0.000262 Mb/s, eth_rx_cnt 1",
      "MTL: 2026-03-04 09:51:28, CNI(1): eth_rx_rate 0.000262 Mb/s, eth_rx_cnt 1",
      "MTL: 2026-03-04 09:51:28, PTP(0): time 1772617888059440198, 2026-03-04 09:51:28",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0:mtl_mxl_poc_rx): fps 59.902375 frames 599 pkts 2468880 redundant -1250275391",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): throughput 5227.151514 Mb/s, cpu busy 1.740302",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): incomplete frames 1, pkts (idx error: 0, offset error: 0, idx out of bitmap: 0, missed: 18)",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): out of order pkts 34",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): succ burst max 94, avg 1.213120",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): tasklet time avg 0.08us max 87.43us min 0.03us",
      "MTL: 2026-03-04 09:51:28, RX_VIDEO_SESSION(0,0): burst avg 0.28us, handler avg 0.10us",
    ];
    const result = parseMtlStatBlock(lines);

    // Structure
    expect(result.timestamp).toBe("2026-03-04 09:51:28");
    expect(result.dev_stats).toHaveLength(2);
    expect(result.sch_stats).toHaveLength(1);
    expect(result.cni_stats).toHaveLength(2);
    expect(result.ptp_stats).toHaveLength(1);
    expect(result.video_sessions).toHaveLength(1);

    // DEV
    expect(result.dev_stats[0].rx_rate_mbps).toBeCloseTo(2604.174342);
    expect(result.dev_stats[1].rx_rate_mbps).toBeCloseTo(2603.837577);

    // SCH extended
    expect(result.sch_stats[0].time_avg_us).toBeCloseTo(0.19);
    expect(result.sch_stats[0].tasklet_details).toHaveLength(1);
    expect(result.sch_stats[0].tasklet_details![0].name).toBe("rvs_pkt_rx");

    // RX session — all 7 bug-fix fields
    const s = result.video_sessions[0];
    expect(s.fps).toBeCloseTo(59.902375);
    expect(s.throughput_mbps).toBeCloseTo(5227.151514);
    expect(s.cpu_busy).toBeCloseTo(1.740302);
    expect(s.incomplete_frames).toBe(1);                   // Bug #2
    expect(s.idx_error).toBe(0);                           // Bug #2
    expect(s.offset_error).toBe(0);                        // Bug #2
    expect(s.idx_out_of_bitmap).toBe(0);                   // Bug #2
    expect(s.missed_pkts).toBe(18);                        // Bug #2
    expect(s.out_of_order_pkts).toBe(34);                  // Bug #3
    expect(s.burst_max).toBe(94);                          // Bug #4
    expect(s.burst_avg).toBeCloseTo(1.21312);              // Bug #4
    expect(s.tasklet_time_avg_us).toBeCloseTo(0.08);       // Bug #6
    expect(s.tasklet_time_max_us).toBeCloseTo(87.43);      // Bug #6
    expect(s.tasklet_time_min_us).toBeCloseTo(0.03);       // Bug #6
  });

  test("parses complete poc synth TX stat dump (TX format)", () => {
    // Exact lines from /dev/shm/poc_logs/synthtx.log on .52
    const lines = [
      "MTL: 2026-03-04 09:51:49, DEV(0): Avr rate, tx: 2612.952115 Mb/s, rx: 0.000000 Mb/s, pkts, tx: 2468253, rx: 0",
      "MTL: 2026-03-04 09:51:49, DEV(1): Avr rate, tx: 2612.882237 Mb/s, rx: 0.000000 Mb/s, pkts, tx: 2468185, rx: 0",
      "MTL: 2026-03-04 09:51:49, SCH(0:sch_0): tasklets 2, lcore 1(t_pid: 619335), avg loop 211 ns",
      "MTL: 2026-03-04 09:51:49, SCH(0): tasklet_calls 94780596, empty_loops 1502096",
      "MTL: 2026-03-04 09:51:49, SCH(0): time avg 0.20us max 284.02us min 0.10us",
      "MTL: 2026-03-04 09:51:49, SCH(0,0): tasklet tx_video_sessions_mgr, avg 0.08us max 283.19us min 0.03us",
      "MTL: 2026-03-04 09:51:49, SCH(0,1): tasklet video_transmitter, avg 0.07us max 67.24us min 0.03us",
      "MTL: 2026-03-04 09:51:49, CNI(0): eth_rx_rate 0.000000 Mb/s, eth_rx_cnt 0",
      "MTL: 2026-03-04 09:51:49, CNI(1): eth_rx_rate 0.000000 Mb/s, eth_rx_cnt 0",
      "MTL: 2026-03-04 09:51:49, PTP(0): time 1772617909432661649, 2026-03-04 09:51:49",
      "MTL: 2026-03-04 09:51:49, TX_VIDEO_SESSION(0,0:synth_tx): fps 59.999709 frames 600 pkts 2469012:2469012 inflight 617323:617403",
      "MTL: 2026-03-04 09:51:49, TX_VIDEO_SESSION(0,0): throughput 2613.743063 Mb/s: 2613.743063 Mb/s, cpu busy 1.350234",
      "MTL: 2026-03-04 09:51:49, TX_VIDEO_SESSION(0,0): 2 frames are in trans, total 2",
      "MTL: 2026-03-04 09:51:49, TX_VIDEO_SESSION(0,0): tasklet time avg 0.01us max 283.11us min 0.01us",
      "MTL: 2026-03-04 09:51:49, TX_VIDEO_SESSION(0,0): alloc avg 0.05us, build avg 0.46us, enqueue avg 0.01us",
      "MTL: 2026-03-04 09:51:49, TX_st20p(0,synth_tx), framebuffer queue: T:2 ",
    ];
    const result = parseMtlStatBlock(lines);

    // SCH with 2 tasklets
    expect(result.sch_stats[0].tasklets).toBe(2);
    expect(result.sch_stats[0].time_avg_us).toBeCloseTo(0.20);
    expect(result.sch_stats[0].tasklet_details).toHaveLength(2);

    // TX session
    const s = result.video_sessions[0];
    expect(s.name).toBe("synth_tx");
    expect(s.fps).toBeCloseTo(59.999709);
    expect(s.throughput_mbps).toBeCloseTo(2613.743063);  // Bug #5: dual Mb/s
    expect(s.cpu_busy).toBeCloseTo(1.350234);
    expect(s.tasklet_time_avg_us).toBeCloseTo(0.01);     // Bug #6: TX tasklet time
    expect(s.tasklet_time_max_us).toBeCloseTo(283.11);
    expect(s.tasklet_time_min_us).toBeCloseTo(0.01);
  });
});
