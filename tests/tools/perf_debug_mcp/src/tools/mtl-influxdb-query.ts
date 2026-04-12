/**
 * mtl_influxdb_query — Query InfluxDB v2 for MTL-related metrics.
 *
 * MTL deployments often feed NIC stats, latency, and counters into InfluxDB via
 * Telegraf or custom scripts.  This tool queries the InfluxDB v2 HTTP API using
 * Flux language.
 *
 * Parameters:
 *   - influxdb_host: InfluxDB host/IP (NOT the SSH host; may be the same)
 *   - port: InfluxDB port (default 8086)
 *   - org: InfluxDB organization (default "")
 *   - bucket: InfluxDB bucket name (REQUIRED)
 *   - measurement: InfluxDB measurement name (REQUIRED)
 *   - token: InfluxDB auth token (default "" for no auth)
 *   - range: Flux time range (default "-5m")
 *   - field_filter: Optional regex to filter fields
 *   - ssh_host: If set, run curl from this remote host via SSH (for firewalled InfluxDB)
 *   - limit: Max number of rows (default 100)
 */
import type { ToolResponse } from "../types.js";
import type { MtlInfluxdbQueryData, InfluxDbSeriesPoint, InfluxDbFieldAggregate } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec, sshExecSafe, sshExecOrThrow } from "../utils/ssh-exec.js";
import { computeStats } from "../utils/helpers.js";
import { execFileSync } from "child_process";

/** Try to auto-discover InfluxDB credentials from machine_env.sh if env vars
 *  are not set.  This is needed because the MCP server is spawned by VS Code
 *  and does not inherit the shell environment from pipeline run scripts. */
let _envLoaded = false;
function loadMachineEnv(): void {
  if (_envLoaded) return;
  _envLoaded = true;
  // Only load if the key env vars are absent
  if (process.env.INFLUXDB_TOKEN && process.env.INFLUXDB_ORG) return;
  try {
    const out: string = execFileSync("bash", ["-c",
      'source /etc/mtl/machine_env.sh 2>/dev/null && echo "T=$INFLUXDB_TOKEN" && echo "O=$INFLUXDB_ORG" && echo "U=$INFLUXDB_URL"',
    ], { encoding: "utf-8", timeout: 3000 });
    for (const line of out.split("\n")) {
      if (line.startsWith("T=") && !process.env.INFLUXDB_TOKEN) {
        process.env.INFLUXDB_TOKEN = line.slice(2).trim();
      } else if (line.startsWith("O=") && !process.env.INFLUXDB_ORG) {
        process.env.INFLUXDB_ORG = line.slice(2).trim();
      } else if (line.startsWith("U=") && !process.env.INFLUXDB_URL) {
        process.env.INFLUXDB_URL = line.slice(2).trim();
      }
    }
  } catch { /* ignore — env discovery is best-effort */ }
}

export async function mtlInfluxdbQuery(params: {
  influxdb_host?: string;
  port?: number;
  org?: string;
  bucket: string;
  measurement: string;
  token?: string;
  range?: string;
  field_filter?: string;
  ssh_host?: string;
  limit?: number;
  aggregate?: boolean;
}): Promise<ToolResponse<MtlInfluxdbQueryData>> {
  loadMachineEnv();
  const influxHost = params.influxdb_host || process.env.INFLUXDB_HOST || "localhost";
  const port = params.port ?? (process.env.INFLUXDB_PORT ? parseInt(process.env.INFLUXDB_PORT, 10) : 8086);
  const org = params.org || process.env.INFLUXDB_ORG || "";
  const bucket = params.bucket;
  const measurement = params.measurement;
  const token = params.token || process.env.INFLUXDB_TOKEN || "";
  const range = params.range ?? "-5m";
  const fieldFilter = params.field_filter;
  const limit = params.limit ?? 100;
  const sshHost = params.ssh_host ?? "";
  const aggregate = params.aggregate ?? false;

  const meta = await buildMeta("fallback");

  try {
    // Build Flux query
    let flux = `from(bucket: "${bucket}")
  |> range(start: ${range})
  |> filter(fn: (r) => r._measurement == "${measurement}")`;

    if (fieldFilter) {
      flux += `\n  |> filter(fn: (r) => r._field =~ /${fieldFilter}/)`;
    }
    flux += `\n  |> limit(n: ${limit})`;

    // Build curl command
    const authHeader = token
      ? `-H "Authorization: Token ${token}"`
      : "";
    const orgParam = org ? `&org=${encodeURIComponent(org)}` : "";
    const url = `http://${influxHost}:${port}/api/v2/query?orgID=${orgParam}`;

    // Use curl with --data-urlencode for the Flux query in text/csv format for easier parsing
    const curlCmd = `curl -s --max-time 10 ${authHeader} -H "Accept: application/csv" -H "Content-Type: application/vnd.flux" -d '${flux.replace(/'/g, "'\\''")}' 'http://${influxHost}:${port}/api/v2/query${org ? "?org=" + encodeURIComponent(org) : ""}'`;

    let csvOutput: string;
    if (sshHost) {
      csvOutput = await sshExecOrThrow(sshHost, curlCmd, 20_000);
    } else {
      // Run locally using bash
      const { execSync } = await import("child_process");
      csvOutput = execSync(curlCmd, {
        encoding: "utf-8",
        timeout: 20_000,
        maxBuffer: 4 * 1024 * 1024,
        shell: "/bin/bash",
      });
    }

    // Parse CSV response
    // InfluxDB CSV columns: result, table, _start, _stop, _time, _value, _field, _measurement, ...tags
    const lines = csvOutput.split("\n").filter((l) => l.trim());

    if (lines.length === 0) {
      return okResponse<MtlInfluxdbQueryData>(
        {
          bucket,
          measurement,
          range,
          series: [],
          row_count: 0,
          truncated: false,
        },
        meta,
      );
    }

    // First non-empty line starting with "" is often annotation, find header row
    let headerIdx = -1;
    for (let i = 0; i < Math.min(lines.length, 5); i++) {
      // Header line has actual column names and starts with empty/result or contains _time
      if (lines[i].includes("_time") || lines[i].includes("_field")) {
        headerIdx = i;
        break;
      }
    }

    if (headerIdx < 0 && lines.length > 0) {
      // Maybe it's an error message
      const firstLine = lines[0];
      if (firstLine.includes("error") || firstLine.includes("unauthorized") || firstLine.includes("not found")) {
        return errorResponse(meta, "INFLUXDB_ERROR", `InfluxDB returned: ${firstLine}`);
      }
      // Try to parse anyway, treating first line as header
      headerIdx = 0;
    }

    const headers = lines[headerIdx].split(",").map((h) => h.trim());
    const series: InfluxDbSeriesPoint[] = [];

    for (let i = headerIdx + 1; i < lines.length; i++) {
      const line = lines[i].trim();
      if (!line || line.startsWith("#") || line.startsWith(",result")) continue;

      const vals = line.split(",");
      if (vals.length < headers.length) continue;

      const record: Record<string, string | number> = {};
      for (let j = 0; j < headers.length; j++) {
        const h = headers[j];
        const v = vals[j]?.trim() ?? "";
        if (!h || h === "" || h === "result" || h === "table") continue;

        // Try numeric conversion for _value
        if (h === "_value") {
          const num = Number(v);
          record[h] = isNaN(num) ? v : num;
        } else {
          record[h] = v;
        }
      }

      if (Object.keys(record).length > 0) {
        series.push({
          time: String(record._time ?? ""),
          field: String(record._field ?? ""),
          value: record._value ?? 0,
          tags: Object.fromEntries(
            Object.entries(record).filter(
              ([k]) => !k.startsWith("_") && k !== "time" && k !== "field" && k !== "value",
            ),
          ) as Record<string, string>,
        });
      }
    }

    const result: MtlInfluxdbQueryData = {
      bucket,
      measurement,
      range,
      series,
      row_count: series.length,
      truncated: series.length >= limit,
    };

    // ── Aggregation (when requested) ──────────────────────────────
    if (aggregate && series.length > 0) {
      const groups = new Map<string, { field: string; tags: Record<string, string>; values: number[] }>();
      for (const pt of series) {
        if (typeof pt.value !== "number") continue;
        const key = pt.field + "|" + JSON.stringify(pt.tags, Object.keys(pt.tags).sort());
        let g = groups.get(key);
        if (!g) {
          g = { field: pt.field, tags: pt.tags, values: [] };
          groups.set(key, g);
        }
        g.values.push(pt.value);
      }
      const aggregates: InfluxDbFieldAggregate[] = [];
      for (const g of groups.values()) {
        const stats = computeStats(g.values);
        if (stats) {
          aggregates.push({
            field: g.field,
            tags: g.tags,
            n: g.values.length,
            ...stats,
          });
        }
      }
      result.aggregates = aggregates;
    }

    return okResponse<MtlInfluxdbQueryData>(result, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_INFLUXDB_QUERY_ERROR",
      `Failed to query InfluxDB: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure InfluxDB is accessible and bucket/measurement exist",
    );
  }
}
