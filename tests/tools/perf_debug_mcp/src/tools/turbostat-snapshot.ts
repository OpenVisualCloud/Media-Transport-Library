/**
 * turbostat_snapshot — Per-core C-state residency, frequency, SMI counts, and power.
 *
 * Reads MSR-based metrics via the Linux `turbostat` binary that are invisible
 * to /proc/stat or sysfs: actual core frequency (MPERF/APERF), C-state
 * residency (C1/C1E/C6), SMI (System Management Interrupt) counts, per-package
 * power via RAPL, and per-core temperature.
 *
 * Key value over existing tools:
 *   - C-state residency: if isolated DPDK cores enter C6, wakeup latency
 *     spikes to 100+ µs — invisible to other tools
 *   - SMI count: invisible OS stalls lasting 1-10 ms per interrupt
 *   - Actual frequency: time-averaged from MSRs, not a sysfs snapshot
 *
 * Requires: `turbostat` binary (linux-tools-$(uname -r)), root/CAP_SYS_RAWIO.
 *
 * Universal — works on any x86 Linux host with turbostat installed.
 */
import type { ToolResponse } from "../types.js";
import type {
  TurbostatSnapshotData,
  TurbostatCoreEntry,
  TurbostatPackageSummary,
  TurbostatWarning,
  TurbostatIpcAnomaly,
} from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/* ── constants ───────────────────────────────────────────────────────────── */

/**
 * Columns we request from turbostat.  Not all columns are available on every
 * CPU — turbostat silently omits unavailable ones.  The parser is column-name
 * driven so absent columns just produce undefined fields.
 */
const TURBO_COLUMNS = [
  "Core", "CPU", "Avg_MHz", "Busy%", "Bzy_MHz", "TSC_MHz",
  "IPC", "IRQ", "SMI",
  "C1%", "C1E%", "C6%",     // C-state residencies (most common on Intel)
  "POLL%",                    // polling idle state
  "CPU%c1", "CPU%c6",        // alternate C-state column names (some kernels)
  "CoreTmp", "PkgTmp",
  "PkgWatt", "RAMWatt",
  "Pkg%pc2", "Pkg%pc6",      // package-level C-state residency
  "UncMHz",                   // uncore frequency
].join(",");

/* ── HT sibling + DPDK core discovery ───────────────────────────────────── */

/**
 * Resolve HT siblings for a set of CPUs.
 * For each physical core, both logical CPUs (threads) are returned.
 */
async function resolveHtSiblings(
  host: string,
  cpus: number[],
): Promise<{ expanded: number[]; siblings_added: number[] }> {
  const cpuSet = new Set(cpus);
  const siblings_added: number[] = [];
  const script = cpus
    .map((c) => `cat /sys/devices/system/cpu/cpu${c}/topology/thread_siblings_list 2>/dev/null || echo "${c}"`)
    .join("; echo '---'; ");
  const output = await sshExecSafe(host, script, 5_000);
  if (!output) return { expanded: cpus, siblings_added: [] };

  const blocks = output.split("---").map((b) => b.trim()).filter(Boolean);
  for (const block of blocks) {
    // thread_siblings_list is comma-separated or range (e.g. "5,117")
    for (const part of block.split(",")) {
      const dashIdx = part.indexOf("-");
      if (dashIdx >= 0) {
        const lo = parseInt(part.slice(0, dashIdx), 10);
        const hi = parseInt(part.slice(dashIdx + 1), 10);
        for (let i = lo; i <= hi; i++) {
          if (!cpuSet.has(i)) { cpuSet.add(i); siblings_added.push(i); }
        }
      } else {
        const n = parseInt(part.trim(), 10);
        if (!isNaN(n) && !cpuSet.has(n)) { cpuSet.add(n); siblings_added.push(n); }
      }
    }
  }
  return { expanded: [...cpuSet].sort((a, b) => a - b), siblings_added: siblings_added.sort((a, b) => a - b) };
}

/**
 * Discover DPDK lcore CPUs from running MTL instances by scanning
 * /proc for mtl_sch_* threads and extracting their CPU affinity.
 */
async function discoverDpdkCores(host: string): Promise<number[]> {
  const script = `for d in /proc/[0-9]*/task/[0-9]*; do
  comm=$(cat "$d/comm" 2>/dev/null)
  case "$comm" in mtl_sch_*|lcore-*|eal-*)
    aff=$(taskset -cp "$(basename "$d")" 2>/dev/null | awk -F': ' '{print $2}')
    echo "$aff"
  ;; esac
done 2>/dev/null`;
  const output = await sshExecSafe(host, script, 10_000);
  if (!output) return [];

  const cpus = new Set<number>();
  for (const line of output.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    for (const part of trimmed.split(",")) {
      const dashIdx = part.indexOf("-");
      if (dashIdx >= 0) {
        const lo = parseInt(part.slice(0, dashIdx), 10);
        const hi = parseInt(part.slice(dashIdx + 1), 10);
        for (let i = lo; i <= hi; i++) cpus.add(i);
      } else {
        const n = parseInt(part.trim(), 10);
        if (!isNaN(n)) cpus.add(n);
      }
    }
  }
  return [...cpus].sort((a, b) => a - b);
}

/**
 * Detect IPC anomalies among DPDK cores — flag cores with IPC deviating
 * >30% from the mean of their peers.
 */
function detectIpcAnomalies(
  cores: TurbostatCoreEntry[],
  filterCpus?: number[],
): TurbostatIpcAnomaly[] {
  const relevantCores = filterCpus
    ? cores.filter((c) => filterCpus.includes(c.cpu))
    : cores;

  // Only look at active cores with IPC data
  const withIpc = relevantCores.filter(
    (c) => c.ipc !== undefined && c.busy_pct !== undefined && c.busy_pct > 5,
  );
  if (withIpc.length < 2) return [];

  const ipcs = withIpc.map((c) => c.ipc!);
  // Use median (robust to outliers) rather than mean for peer comparison
  const sorted = [...ipcs].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  const median = sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
  if (median === 0) return [];

  const anomalies: TurbostatIpcAnomaly[] = [];
  for (const c of withIpc) {
    const deviation = ((c.ipc! - median) / median) * 100;
    if (Math.abs(deviation) > 30) {
      anomalies.push({
        cpu: c.cpu,
        metric: "IPC",
        value: Math.round(c.ipc! * 100) / 100,
        peer_avg: Math.round(median * 100) / 100,
        deviation_pct: Math.round(deviation),
        message: `CPU ${c.cpu}: IPC ${c.ipc!.toFixed(2)} deviates ${Math.abs(Math.round(deviation))}% from peer median ${median.toFixed(2)}`,
      });
    }
  }
  return anomalies;
}

/* ── parser ──────────────────────────────────────────────────────────────── */

/**
 * Parse turbostat TSV-like stdout into structured objects.
 *
 * turbostat outputs tab-separated values.  The first row is the header.
 * Rows with Core="-" and CPU="-" are system-wide summaries (one per package
 * or overall).
 */
function parseTurbostatOutput(output: string): {
  headers: string[];
  summary: Record<string, string>;
  cores: Array<Record<string, string>>;
} {
  const lines = output.split("\n").filter((l) => l.trim().length > 0);
  if (lines.length < 2) return { headers: [], summary: {}, cores: [] };

  const headers = lines[0].split("\t");
  const summary: Record<string, string> = {};
  const cores: Array<Record<string, string>> = [];

  for (let i = 1; i < lines.length; i++) {
    const fields = lines[i].split("\t");
    const row: Record<string, string> = {};
    for (let j = 0; j < headers.length; j++) {
      row[headers[j]] = fields[j] ?? "";
    }

    const coreVal = row["Core"] ?? "";
    const cpuVal = row["CPU"] ?? "";

    if (coreVal === "-" && cpuVal === "-") {
      // System-wide or package summary
      Object.assign(summary, row);
    } else {
      cores.push(row);
    }
  }

  return { headers, summary, cores };
}

function safeFloat(val: string | undefined): number | undefined {
  if (val === undefined || val === "" || val === "-") return undefined;
  const n = parseFloat(val);
  return isNaN(n) ? undefined : n;
}

function safeInt(val: string | undefined): number | undefined {
  if (val === undefined || val === "" || val === "-") return undefined;
  const n = parseInt(val, 10);
  return isNaN(n) ? undefined : n;
}

function buildCoreEntry(row: Record<string, string>): TurbostatCoreEntry {
  return {
    core: safeInt(row["Core"]) ?? -1,
    cpu: safeInt(row["CPU"]) ?? -1,
    avg_mhz: safeFloat(row["Avg_MHz"]),
    busy_pct: safeFloat(row["Busy%"]),
    bzy_mhz: safeFloat(row["Bzy_MHz"]),
    tsc_mhz: safeFloat(row["TSC_MHz"]),
    ipc: safeFloat(row["IPC"]),
    irq: safeInt(row["IRQ"]),
    smi: safeInt(row["SMI"]),
    c1_pct: safeFloat(row["C1%"]) ?? safeFloat(row["CPU%c1"]),
    c1e_pct: safeFloat(row["C1E%"]),
    c6_pct: safeFloat(row["C6%"]) ?? safeFloat(row["CPU%c6"]),
    poll_pct: safeFloat(row["POLL%"]),
    core_tmp_c: safeInt(row["CoreTmp"]),
  };
}

function buildPackageSummary(row: Record<string, string>): TurbostatPackageSummary {
  return {
    avg_mhz: safeFloat(row["Avg_MHz"]),
    busy_pct: safeFloat(row["Busy%"]),
    bzy_mhz: safeFloat(row["Bzy_MHz"]),
    tsc_mhz: safeFloat(row["TSC_MHz"]),
    ipc: safeFloat(row["IPC"]),
    total_smi: safeInt(row["SMI"]),
    c1_pct: safeFloat(row["C1%"]) ?? safeFloat(row["CPU%c1"]),
    c1e_pct: safeFloat(row["C1E%"]),
    c6_pct: safeFloat(row["C6%"]) ?? safeFloat(row["CPU%c6"]),
    pkg_tmp_c: safeInt(row["PkgTmp"]),
    pkg_watt: safeFloat(row["PkgWatt"]),
    ram_watt: safeFloat(row["RAMWatt"]),
    pkg_pc2_pct: safeFloat(row["Pkg%pc2"]),
    pkg_pc6_pct: safeFloat(row["Pkg%pc6"]),
    unc_mhz: safeFloat(row["UncMHz"]),
  };
}

/* ── warning generation ──────────────────────────────────────────────────── */

function generateWarnings(
  cores: TurbostatCoreEntry[],
  summary: TurbostatPackageSummary,
  coreFilter?: number[],
): TurbostatWarning[] {
  const warnings: TurbostatWarning[] = [];
  const filterSet = coreFilter ? new Set(coreFilter) : null;

  // Check cores of interest (filtered or all)
  for (const c of cores) {
    if (filterSet && !filterSet.has(c.cpu)) continue;

    // SMI detected
    if (c.smi !== undefined && c.smi > 0) {
      warnings.push({
        cpu: c.cpu,
        severity: "critical",
        message: `CPU ${c.cpu}: ${c.smi} SMI(s) detected — invisible OS stalls (1-10 ms each)`,
      });
    }

    // Deep C-state on a potentially isolated core
    if (c.c6_pct !== undefined && c.c6_pct > 1.0) {
      warnings.push({
        cpu: c.cpu,
        severity: "warning",
        message: `CPU ${c.cpu}: ${c.c6_pct.toFixed(1)}% C6 residency — deep sleep causes 100+ µs wakeup latency`,
      });
    }

    // Very low frequency relative to TSC (throttling)
    if (c.bzy_mhz !== undefined && c.tsc_mhz !== undefined && c.bzy_mhz < c.tsc_mhz * 0.5 && c.busy_pct !== undefined && c.busy_pct > 10) {
      warnings.push({
        cpu: c.cpu,
        severity: "warning",
        message: `CPU ${c.cpu}: frequency ${c.bzy_mhz} MHz is well below base ${c.tsc_mhz} MHz (possible thermal throttling)`,
      });
    }

    // Core temperature alarm
    if (c.core_tmp_c !== undefined && c.core_tmp_c >= 95) {
      warnings.push({
        cpu: c.cpu,
        severity: "critical",
        message: `CPU ${c.cpu}: core temperature ${c.core_tmp_c}°C — thermal throttling imminent`,
      });
    }
  }

  // Package-level warnings
  if (summary.total_smi !== undefined && summary.total_smi > 0) {
    warnings.push({
      cpu: -1,
      severity: "critical",
      message: `System: ${summary.total_smi} total SMIs detected across all cores`,
    });
  }

  if (summary.pkg_tmp_c !== undefined && summary.pkg_tmp_c >= 90) {
    warnings.push({
      cpu: -1,
      severity: "warning",
      message: `Package temperature ${summary.pkg_tmp_c}°C — approaching thermal limit`,
    });
  }

  return warnings;
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function turbostatSnapshot(params: {
  host?: string;
  interval_sec?: number;
  cpu_filter?: number[];
  dpdk_only?: boolean;
}): Promise<ToolResponse<TurbostatSnapshotData>> {
  const host = params.host ?? "localhost";
  const interval = params.interval_sec ?? 1;
  let cpuFilter = params.cpu_filter;
  const dpdkOnly = params.dpdk_only ?? false;

  // dpdk_only mode: auto-detect DPDK scheduler cores
  let dpdkCoresDetected: number[] | undefined;
  if (dpdkOnly) {
    dpdkCoresDetected = await discoverDpdkCores(host);
    if (dpdkCoresDetected.length > 0) {
      cpuFilter = dpdkCoresDetected;
    }
  }

  // Resolve HT siblings for the filter set
  let requestedCpus: number[] | undefined;
  let htSiblingsAdded: number[] | undefined;
  if (cpuFilter && cpuFilter.length > 0) {
    requestedCpus = [...cpuFilter];
    const { expanded, siblings_added } = await resolveHtSiblings(host, cpuFilter);
    cpuFilter = expanded;
    if (siblings_added.length > 0) htSiblingsAdded = siblings_added;
  }

  const meta = await buildMeta("fallback", interval * 1000);

  // Check turbostat availability
  const check = await sshExecSafe(host, "command -v turbostat 2>/dev/null");
  if (!check || !check.trim()) {
    return errorResponse(
      meta,
      "TURBOSTAT_MISSING",
      "turbostat not found on target host",
      "Install linux-tools for your kernel: apt-get install linux-tools-$(uname -r) linux-tools-common",
    );
  }

  try {
    // Build turbostat command
    // --show limits output columns, --interval sets measurement window,
    // --num_iterations 1 makes it exit after one measurement
    const cmd = `turbostat --show ${TURBO_COLUMNS} --interval ${interval} --num_iterations 1 2>/dev/null`;

    const timeoutMs = (interval + 10) * 1000;
    const output = await sshExecSafe(host, cmd, timeoutMs);
    if (!output || !output.trim()) {
      return errorResponse(
        meta,
        "TURBOSTAT_NO_OUTPUT",
        "turbostat produced no output",
        "Ensure turbostat can access MSRs: run as root or with CAP_SYS_RAWIO. Check that msr module is loaded: modprobe msr",
      );
    }

    // Parse output
    const { headers, summary: summaryRow, cores: coreRows } = parseTurbostatOutput(output);

    if (headers.length === 0) {
      return errorResponse(meta, "TURBOSTAT_PARSE_ERROR", "Failed to parse turbostat output — no header found");
    }

    // Build structured data
    const allCores = coreRows.map(buildCoreEntry);
    const packageSummary = buildPackageSummary(summaryRow);

    // Apply CPU filter
    let filteredCores = allCores;
    if (cpuFilter && cpuFilter.length > 0) {
      const filterSet = new Set(cpuFilter);
      filteredCores = allCores.filter((c) => filterSet.has(c.cpu));
    }

    // Sort by CPU id
    filteredCores.sort((a, b) => a.cpu - b.cpu);

    const warnings = generateWarnings(allCores, packageSummary, cpuFilter);
    const ipcAnomalies = detectIpcAnomalies(allCores, cpuFilter);

    const result: TurbostatSnapshotData = {
      interval_sec: interval,
      columns_available: headers,
      package_summary: packageSummary,
      cores: filteredCores,
      core_count: filteredCores.length,
      total_cores_on_system: allCores.length,
      warnings,
    };

    if (requestedCpus) result.requested_cpus = requestedCpus;
    if (htSiblingsAdded && htSiblingsAdded.length > 0) result.ht_siblings_added = htSiblingsAdded;
    if (dpdkCoresDetected && dpdkCoresDetected.length > 0) result.dpdk_cores_detected = dpdkCoresDetected;
    if (ipcAnomalies.length > 0) result.ipc_anomalies = ipcAnomalies;

    return okResponse<TurbostatSnapshotData>(result, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "TURBOSTAT_ERROR",
      `Failed to run turbostat: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure turbostat is installed and you have root access for MSR reads",
    );
  }
}
