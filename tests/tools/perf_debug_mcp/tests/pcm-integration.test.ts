/**
 * Integration tests for Intel PCM tools.
 *
 * These tests exercise the 7 PCM tools against a live pcm-sensor-server.
 * If pcm-sensor-server is not running, the tests verify graceful degradation
 * (structured error response with hint).
 *
 * To run with a live server: start pcm-sensor-server on port 9738, then `npm test`.
 * To run without: tests validate error responses instead.
 */
import { describe, test, expect, beforeAll } from "@jest/globals";
import { getPcmBridge } from "../src/collectors/pcm-bridge.js";
import { pcmCoreCounters } from "../src/tools/pcm-core-counters.js";
import { pcmMemoryBandwidth } from "../src/tools/pcm-memory-bandwidth.js";
import { pcmCacheAnalysis } from "../src/tools/pcm-cache-analysis.js";
import { pcmPowerThermal } from "../src/tools/pcm-power-thermal.js";
import { pcmQpiUpiLink } from "../src/tools/pcm-qpi-upi-link.js";
import { pcmPcieBandwidth } from "../src/tools/pcm-pcie-bandwidth.js";
import { pcmNumaTraffic } from "../src/tools/pcm-numa-traffic.js";

let pcmAvailable = false;

beforeAll(async () => {
  const bridge = getPcmBridge();
  await bridge.detect();
  pcmAvailable = bridge.isAvailable;
  if (!pcmAvailable) {
    console.warn("pcm-sensor-server not detected — PCM tests will verify graceful error handling");
  }
});

// ─── Helper to validate response envelope ──────────────────────────────────

function expectOk(r: any) {
  expect(r.ok).toBe(true);
  expect(r.meta).toBeDefined();
  expect(r.meta.timestamp_wall).toBeDefined();
  expect(typeof r.meta.t_monotonic_ns).toBe("number");
  expect(r.data).toBeDefined();
}

function expectPcmUnavailableError(r: any) {
  expect(r.ok).toBe(false);
  expect(r.error).toBeDefined();
  expect(r.error.code).toBe("PCM_UNAVAILABLE");
  expect(typeof r.error.message).toBe("string");
  // Should include a helpful hint about starting pcm-sensor-server
  expect(r.error.message.toLowerCase()).toMatch(/pcm-sensor-server|not available|not running/);
}

// ─── pcm_core_counters ─────────────────────────────────────────────────────

describe("pcm_core_counters", () => {
  test("returns per-core data or graceful error", async () => {
    const r = await pcmCoreCounters({ seconds: 1, socket_filter: null, core_filter: null });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.cores)).toBe(true);
      if (r.data!.cores.length > 0) {
        const c = r.data!.cores[0];
        expect(typeof c.socket).toBe("number");
        expect(typeof c.core).toBe("number");
        expect(typeof c.ipc).toBe("number");
        expect(typeof c.instructions_retired).toBe("number");
      }
      // Aggregates
      expect(r.data!.aggregates).toBeDefined();
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);

  test("socket_filter works", async () => {
    const r = await pcmCoreCounters({ seconds: 1, socket_filter: 0, core_filter: null });
    if (pcmAvailable) {
      expectOk(r);
      for (const c of r.data!.cores) {
        expect(c.socket).toBe(0);
      }
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);

  test("core_filter works", async () => {
    const r = await pcmCoreCounters({ seconds: 1, socket_filter: null, core_filter: "0" });
    if (pcmAvailable) {
      expectOk(r);
      for (const c of r.data!.cores) {
        expect(c.core).toBe(0);
      }
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── pcm_memory_bandwidth ──────────────────────────────────────────────────

describe("pcm_memory_bandwidth", () => {
  test("returns per-socket memory data or graceful error", async () => {
    const r = await pcmMemoryBandwidth({ seconds: 1, socket_filter: null });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.sockets)).toBe(true);
      if (r.data!.sockets.length > 0) {
        const s = r.data!.sockets[0];
        expect(typeof s.socket).toBe("number");
        expect(typeof s.dram_reads_bytes).toBe("number");
        expect(typeof s.dram_writes_bytes).toBe("number");
      }
      expect(typeof r.data!.interval_us).toBe("number");
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── pcm_cache_analysis ────────────────────────────────────────────────────

describe("pcm_cache_analysis", () => {
  test("returns cache analysis or graceful error", async () => {
    const r = await pcmCacheAnalysis({ seconds: 1, socket_filter: null, core_filter: null });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.cores)).toBe(true);
      if (r.data!.cores.length > 0) {
        const c = r.data!.cores[0];
        expect(typeof c.socket).toBe("number");
        expect(typeof c.core).toBe("number");
        expect(typeof c.l2_hit_ratio).toBe("number");
        expect(typeof c.l3_hit_ratio).toBe("number");
        // Hit ratios should be between 0 and 1
        expect(c.l2_hit_ratio).toBeGreaterThanOrEqual(0);
        expect(c.l2_hit_ratio).toBeLessThanOrEqual(1);
        expect(c.l3_hit_ratio).toBeGreaterThanOrEqual(0);
        expect(c.l3_hit_ratio).toBeLessThanOrEqual(1);
      }
      expect(typeof r.data!.system_l3_hit_ratio).toBe("number");
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── pcm_power_thermal ─────────────────────────────────────────────────────

describe("pcm_power_thermal", () => {
  test("returns power/thermal data or graceful error", async () => {
    const r = await pcmPowerThermal({ seconds: 1, socket_filter: null, include_tma: true });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.cores)).toBe(true);
      expect(Array.isArray(r.data!.sockets)).toBe(true);
      if (r.data!.cores.length > 0) {
        const c = r.data!.cores[0];
        expect(typeof c.socket).toBe("number");
        expect(typeof c.core).toBe("number");
        expect(typeof c.thermal_headroom_c).toBe("number");
      }
      // TMA should be present
      expect(r.data!.tma).toBeDefined();
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);

  test("include_tma=false excludes TMA", async () => {
    const r = await pcmPowerThermal({ seconds: 1, socket_filter: null, include_tma: false });
    if (pcmAvailable) {
      expectOk(r);
      expect(r.data!.tma).toBeUndefined();
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── pcm_qpi_upi_link ─────────────────────────────────────────────────────

describe("pcm_qpi_upi_link", () => {
  test("returns QPI/UPI data or graceful error", async () => {
    const r = await pcmQpiUpiLink({ seconds: 1 });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.sockets)).toBe(true);
      // On single-socket systems, sockets array may be empty or have empty links
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── pcm_pcie_bandwidth ────────────────────────────────────────────────────

describe("pcm_pcie_bandwidth", () => {
  test("returns PCIe bandwidth or graceful error", async () => {
    const r = await pcmPcieBandwidth({ seconds: 1, socket_filter: null });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.sockets)).toBe(true);
      if (r.data!.sockets.length > 0) {
        const s = r.data!.sockets[0];
        expect(typeof s.socket).toBe("number");
      }
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── pcm_numa_traffic ──────────────────────────────────────────────────────

describe("pcm_numa_traffic", () => {
  test("returns NUMA traffic or graceful error", async () => {
    const r = await pcmNumaTraffic({ seconds: 1, socket_filter: null, top_n: 0 });
    if (pcmAvailable) {
      expectOk(r);
      expect(Array.isArray(r.data!.per_core)).toBe(true);
      expect(Array.isArray(r.data!.per_socket)).toBe(true);
      if (r.data!.per_core.length > 0) {
        const c = r.data!.per_core[0];
        expect(typeof c.socket).toBe("number");
        expect(typeof c.core).toBe("number");
        expect(typeof c.local_memory_bw_bytes).toBe("number");
        expect(typeof c.remote_memory_bw_bytes).toBe("number");
        expect(typeof c.local_ratio_pct).toBe("number");
      }
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);

  test("top_n limits output", async () => {
    const r = await pcmNumaTraffic({ seconds: 1, socket_filter: null, top_n: 3 });
    if (pcmAvailable) {
      expectOk(r);
      expect(r.data!.per_core.length).toBeLessThanOrEqual(3);
    } else {
      expectPcmUnavailableError(r);
    }
  }, 15000);
});

// ─── capabilities includes PCM status ──────────────────────────────────────

describe("capabilities includes PCM", () => {
  test("available_tools includes PCM tools", async () => {
    const { capabilities } = await import("../src/tools/capabilities.js");
    const r = await capabilities();
    expect(r.ok).toBe(true);
    const tools = r.data!.available_tools;
    expect(tools).toContain("pcm_core_counters");
    expect(tools).toContain("pcm_memory_bandwidth");
    expect(tools).toContain("pcm_cache_analysis");
    expect(tools).toContain("pcm_power_thermal");
    expect(tools).toContain("pcm_qpi_upi_link");
    expect(tools).toContain("pcm_pcie_bandwidth");
    expect(tools).toContain("pcm_numa_traffic");
    expect(tools.length).toBeGreaterThanOrEqual(26); // 19 original + 7 PCM
  });

  test("has pcm_server_available field", async () => {
    const { capabilities } = await import("../src/tools/capabilities.js");
    const r = await capabilities();
    expect(r.ok).toBe(true);
    expect(typeof r.data!.pcm_server_available).toBe("boolean");
    expect(typeof r.data!.pcm_server_endpoint).toBe("string");
  });
});
