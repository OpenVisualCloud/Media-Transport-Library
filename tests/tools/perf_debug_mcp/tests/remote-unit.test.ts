/**
 * Tests for remote infrastructure: host-utils, agent-script parsing, agent-manager.
 */
import { describe, test, expect, beforeAll } from "@jest/globals";

// ── host-utils ──────────────────────────────────────────────────

describe("isLocalHost", () => {
  let isLocalHost: typeof import("../src/remote/host-utils.js").isLocalHost;

  beforeAll(async () => {
    const mod = await import("../src/remote/host-utils.js");
    isLocalHost = mod.isLocalHost;
  });

  test("undefined → true", () => {
    expect(isLocalHost(undefined)).toBe(true);
  });

  test("empty string → true", () => {
    expect(isLocalHost("")).toBe(true);
  });

  test("localhost → true", () => {
    expect(isLocalHost("localhost")).toBe(true);
  });

  test("127.0.0.1 → true", () => {
    expect(isLocalHost("127.0.0.1")).toBe(true);
  });

  test("remote IP → false", () => {
    expect(isLocalHost("192.0.2.30")).toBe(false);
  });

  test("remote hostname → false", () => {
    expect(isLocalHost("compute-node-1")).toBe(false);
  });
});

// ── agent-manager section parser ────────────────────────────────

describe("parseAgentSections", () => {
  let parseAgentSections: typeof import("../src/remote/agent-manager.js").parseAgentSections;

  beforeAll(async () => {
    const mod = await import("../src/remote/agent-manager.js");
    parseAgentSections = mod.parseAgentSections;
  });

  test("parses TASKS section", () => {
    const raw = [
      "===CPU_DBG_AGENT===TASKS",
      "1 (systemd) S 0 1 1 0 -1 4194560 12345 6789",
      "2 (kthreadd) S 0 0 0 0 -1 2129984 0 0",
      "===CPU_DBG_AGENT===END",
    ].join("\n");

    const sections = parseAgentSections(raw);
    expect(sections.has("TASKS")).toBe(true);
    const tasks = sections.get("TASKS")!;
    expect(tasks).toContain("systemd");
    expect(tasks).toContain("kthreadd");
    expect(sections.has("END")).toBe(true);
  });

  test("parses AFFINITIES with TID sub-sections", () => {
    const raw = [
      "===CPU_DBG_AGENT===AFFINITIES",
      "===CPU_DBG_AGENT===TID 1234",
      "Cpus_allowed:\tff",
      "Cpus_allowed_list:\t0-7",
      "Cpuset_cpus: 0-3",
      "===CPU_DBG_AGENT===TID 5678",
      "Cpus_allowed:\t0f",
      "Cpus_allowed_list:\t0-3",
      "===CPU_DBG_AGENT===END",
    ].join("\n");

    const sections = parseAgentSections(raw);
    expect(sections.has("TID 1234")).toBe(true);
    expect(sections.has("TID 5678")).toBe(true);

    const tid1 = sections.get("TID 1234")!;
    expect(tid1).toContain("Cpus_allowed:\tff");
    expect(tid1).toContain("Cpuset_cpus: 0-3");

    const tid2 = sections.get("TID 5678")!;
    expect(tid2).toContain("Cpus_allowed_list:\t0-3");
    expect(tid2).not.toContain("Cpuset_cpus");
  });

  test("parses SYSFS + FREQ + NUMA sections", () => {
    const raw = [
      "===CPU_DBG_AGENT===SYSFS",
      "online=0-31",
      "offline=",
      "isolated=4-15",
      "cmdline=isolcpus=4-15 nohz_full=4-15",
      "===CPU_DBG_AGENT===FREQ",
      "0 3600000 800000 3700000 performance 0",
      "1 3600000 800000 3700000 performance 0",
      "===CPU_DBG_AGENT===NUMA",
      "0 0-15 10 21",
      "1 16-31 21 10",
      "===CPU_DBG_AGENT===END",
    ].join("\n");

    const sections = parseAgentSections(raw);
    expect(sections.has("SYSFS")).toBe(true);
    expect(sections.has("FREQ")).toBe(true);
    expect(sections.has("NUMA")).toBe(true);

    expect(sections.get("SYSFS")).toContain("online=0-31");
    expect(sections.get("SYSFS")).toContain("isolated=4-15");
    expect(sections.get("FREQ")).toContain("3600000");
    expect(sections.get("NUMA")).toContain("0-15");
  });

  test("empty output returns empty map", () => {
    const sections = parseAgentSections("");
    expect(sections.size).toBe(0);
  });
});

// ── remoteAffinities parsing ────────────────────────────────────

describe("remoteAffinities parsing", () => {
  let remoteAffinities: typeof import("../src/remote/agent-manager.js").remoteAffinities;

  test("is exported", async () => {
    const mod = await import("../src/remote/agent-manager.js");
    expect(typeof mod.remoteAffinities).toBe("function");
  });
});

// ── remoteSysfs parsing ─────────────────────────────────────────

describe("remoteSysfs result types", () => {
  test("module exports remoteSysfs", async () => {
    const mod = await import("../src/remote/agent-manager.js");
    expect(typeof mod.remoteSysfs).toBe("function");
  });
});

// ── Agent script ────────────────────────────────────────────────

describe("agent script", () => {
  let AGENT_SCRIPT: string;
  let AGENT_VERSION: string;

  beforeAll(async () => {
    const mod = await import("../src/remote/agent-script.js");
    AGENT_SCRIPT = mod.AGENT_SCRIPT;
    AGENT_VERSION = mod.AGENT_VERSION;
  });

  test("version is defined", () => {
    expect(AGENT_VERSION).toBeDefined();
    expect(AGENT_VERSION.length).toBeGreaterThan(0);
  });

  test("script starts with shebang", () => {
    expect(AGENT_SCRIPT.startsWith("#!/bin/bash")).toBe(true);
  });

  test("script contains all commands", () => {
    expect(AGENT_SCRIPT).toContain("tasks)");
    expect(AGENT_SCRIPT).toContain("affinities)");
    expect(AGENT_SCRIPT).toContain("sysfs)");
    expect(AGENT_SCRIPT).toContain("version)");
  });

  test("script uses correct delimiter", () => {
    expect(AGENT_SCRIPT).toContain("===CPU_DBG_AGENT===");
  });
});

// ── Collector host passthrough (local mode) ─────────────────────

describe("collectors accept host? param (local mode)", () => {
  test("readProcStat with no host works", async () => {
    const { readProcStat } = await import("../src/collectors/proc-stat.js");
    const result = await readProcStat();
    expect(result.length).toBeGreaterThan(0);
    expect(result[0].cpu).toBeDefined();
  });

  test("readProcStat with localhost works", async () => {
    const { readProcStat } = await import("../src/collectors/proc-stat.js");
    const result = await readProcStat("localhost");
    expect(result.length).toBeGreaterThan(0);
  });

  test("readProcSoftirqs with no host works", async () => {
    const { readProcSoftirqs } = await import("../src/collectors/softirqs.js");
    const result = await readProcSoftirqs();
    expect(result.cpu_count).toBeGreaterThan(0);
    expect(result.lines.length).toBeGreaterThan(0);
  });

  test("readProcInterrupts with no host works", async () => {
    const { readProcInterrupts } = await import("../src/collectors/interrupts.js");
    const result = await readProcInterrupts();
    expect(result.cpu_count).toBeGreaterThan(0);
    expect(result.lines.length).toBeGreaterThan(0);
  });

  test("getOnlineCpus with no host works", async () => {
    const { getOnlineCpus } = await import("../src/collectors/sysfs.js");
    const result = await getOnlineCpus();
    expect(result.length).toBeGreaterThan(0);
  });

  test("getOnlineCpus with localhost works", async () => {
    const { getOnlineCpus } = await import("../src/collectors/sysfs.js");
    const result = await getOnlineCpus("localhost");
    expect(result.length).toBeGreaterThan(0);
  });

  test("buildTaskTable with host=undefined works", async () => {
    const { buildTaskTable } = await import("../src/collectors/proc-pid-stat.js");
    const result = await buildTaskTable({ limit: 10 });
    expect(result.length).toBeGreaterThan(0);
    expect(result[0].pid).toBeDefined();
  });
});

// ── Host-aware utility functions ────────────────────────────────

describe("readFileHost local mode", () => {
  let readFileHost: typeof import("../src/remote/host-utils.js").readFileHost;
  let readFileTrimmedHost: typeof import("../src/remote/host-utils.js").readFileTrimmedHost;
  let listEntriesHost: typeof import("../src/remote/host-utils.js").listEntriesHost;
  let isReadableHost: typeof import("../src/remote/host-utils.js").isReadableHost;
  let readFilesHost: typeof import("../src/remote/host-utils.js").readFilesHost;

  beforeAll(async () => {
    const mod = await import("../src/remote/host-utils.js");
    readFileHost = mod.readFileHost;
    readFileTrimmedHost = mod.readFileTrimmedHost;
    listEntriesHost = mod.listEntriesHost;
    isReadableHost = mod.isReadableHost;
    readFilesHost = mod.readFilesHost;
  });

  test("readFileHost local reads /proc/stat", async () => {
    const content = await readFileHost(undefined, "/proc/stat");
    expect(content).toContain("cpu ");
  });

  test("readFileTrimmedHost local reads /proc/loadavg", async () => {
    const content = await readFileTrimmedHost(undefined, "/proc/loadavg");
    expect(content).toBeTruthy();
    expect(content!.split(" ").length).toBeGreaterThanOrEqual(3);
  });

  test("readFileTrimmedHost returns null for missing file", async () => {
    const content = await readFileTrimmedHost(undefined, "/proc/nonexistent_file_xyz");
    expect(content).toBeNull();
  });

  test("listEntriesHost lists /proc/irq", async () => {
    const entries = await listEntriesHost(undefined, "/proc/irq");
    expect(entries.length).toBeGreaterThan(0);
    expect(entries.some((e) => /^\d+$/.test(e))).toBe(true);
  });

  test("isReadableHost returns true for /proc/stat", async () => {
    const result = await isReadableHost(undefined, "/proc/stat");
    expect(result).toBe(true);
  });

  test("isReadableHost returns false for nonexistent path", async () => {
    const result = await isReadableHost(undefined, "/nonexistent_path_xyz");
    expect(result).toBe(false);
  });

  test("readFilesHost batch reads multiple files", async () => {
    const paths = ["/proc/stat", "/proc/softirqs", "/proc/nonexistent_xyz"];
    const result = await readFilesHost(undefined, paths);
    expect(result.get("/proc/stat")).toContain("cpu ");
    expect(result.get("/proc/softirqs")).toContain("CPU");
    expect(result.get("/proc/nonexistent_xyz")).toBeNull();
  });
});
