/**
 * log_search — Structured search across pipeline log files.
 *
 * Features deduplication, timestamp parsing, and severity classification.
 * Distinguishes real errors from MTL informational stat dump lines that use
 * the "Error:" prefix for NIC counter reports.
 *
 * Data sources:
 *   - Log files in /dev/shm/*_logs/ (or user-specified dirs/paths)
 *   - MTL stat dump blocks (M T D E V S T A T E markers)
 *
 * Universal — works with any MTL deployment's log layout.
 */
import type { ToolResponse } from "../types.js";
import type {
  LogSearchData,
  LogSearchMatch,
  LogSearchFileResult,
} from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

/* ── constants ───────────────────────────────────────────────────────────── */

/** Default error pattern when none provided */
const DEFAULT_ERROR_PATTERN = "fail(?!over)|timeout|abort|fatal|panic|exception|refused|broken.?pipe|segfault";

/** Lines that match "Error:" but are actually MTL stat dump NIC counter reports */
const STAT_DUMP_PATTERNS: RegExp[] = [
  /MTL:.*Error:.*Status:.*_packets/i,
  /MTL:.*Error:.*(?:rx_good_|tx_good_|rx_bytes|rx_multicast|rx_nombuf)/i,
  /MTL:.*Error:.*SCH\(/i,
  /MTL:.*Error:.*(?:rx_err_packets|tx_err_packets|rx_hw_dropped)/i,
];

/** Warning patterns (case-insensitive) */
const WARNING_RE = /\b(warn(?:ing)?|deprecated|retry|slow|latency\s+spike|backpressure|overflow|underrun)\b/i;

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Classify a log line.
 */
function classifyLine(
  line: string,
  inStatBlock: boolean,
  patternRe: RegExp | null,
): "error" | "warning" | "info" | "stat_dump" | null {
  // Inside a stat dump block
  if (inStatBlock) return "stat_dump";

  // Check if it's a stat dump line masquerading as error
  for (const re of STAT_DUMP_PATTERNS) {
    if (re.test(line)) return "stat_dump";
  }

  // Check for real error pattern
  if (patternRe && patternRe.test(line)) return "error";

  // Check for warning pattern
  if (WARNING_RE.test(line)) return "warning";

  return null;
}

/**
 * Parse timestamp from an MTL log line.
 */
function parseTimestamp(line: string): string | undefined {
  // MTL: YYYY-MM-DD HH:MM:SS
  const m = line.match(/MTL:\s*(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2})/);
  if (m) return `${m[1]}T${m[2]}`;

  // ISO 8601 timestamp
  const iso = line.match(/(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})/);
  if (iso) return iso[1];

  return undefined;
}

/**
 * Normalize a line for deduplication: strip timestamp and replace numbers with #.
 */
function normalizeLine(line: string): string {
  return line
    .replace(/\d{4}-\d{2}-\d{2}[\sT]\d{2}:\d{2}:\d{2}[.\d]*/g, "TIMESTAMP")
    .replace(/\b\d+\b/g, "#")
    .replace(/\s+/g, " ")
    .trim();
}

/**
 * Discover log files from directories.
 */
async function discoverLogFiles(
  host: string,
  logDirs?: string[],
  logPaths?: string[],
): Promise<string[]> {
  const files: string[] = [];

  // Explicit paths first
  if (logPaths && logPaths.length > 0) {
    files.push(...logPaths);
  }

  // Directory scanning
  let cmd: string;
  if (logDirs && logDirs.length > 0) {
    const finds = logDirs.map((d) => `find "${d}" -name '*.log' -maxdepth 2 2>/dev/null`).join("; ");
    cmd = finds;
  } else if (!logPaths || logPaths.length === 0) {
    // Default: discover from /dev/shm
    cmd = `find /dev/shm -maxdepth 2 -name '*.log' -path '*_logs/*' 2>/dev/null`;
  } else {
    return files; // Only explicit paths, no directory scan
  }

  const output = await sshExecSafe(host, cmd, 10_000);
  if (output) {
    for (const f of output.trim().split("\n").filter(Boolean)) {
      if (!files.includes(f)) files.push(f);
    }
  }

  return files.sort();
}

/* ── main entry ──────────────────────────────────────────────────────────── */

export async function logSearch(params: {
  host?: string;
  log_dirs?: string[];
  log_paths?: string[];
  pattern?: string;
  minutes?: number;
  severity?: "error" | "warning" | "all";
  exclude_stat_dumps?: boolean;
  max_results?: number;
  dedup?: boolean;
}): Promise<ToolResponse<LogSearchData>> {
  const host = params.host ?? "localhost";
  const minutes = params.minutes ?? 5;
  const severity = params.severity ?? "error";
  const excludeStatDumps = params.exclude_stat_dumps ?? true;
  const maxResults = params.max_results ?? 50;
  const dedup = params.dedup ?? true;

  const meta = await buildMeta("fallback");

  try {
    // Build search pattern
    const patternStr = params.pattern ?? DEFAULT_ERROR_PATTERN;
    let patternRe: RegExp | null;
    try {
      patternRe = new RegExp(patternStr, "i");
    } catch {
      return errorResponse(meta, "INVALID_PATTERN", `Invalid regex pattern: ${patternStr}`);
    }

    // Discover log files
    const logFiles = await discoverLogFiles(host, params.log_dirs, params.log_paths);
    if (logFiles.length === 0) {
      return okResponse<LogSearchData>(
        { matches: [], match_count: 0, truncated: false, files: [], files_scanned: 0 },
        meta,
      );
    }

    const cutoffMs = Date.now() - minutes * 60_000;
    const allMatches: LogSearchMatch[] = [];
    const fileResults: LogSearchFileResult[] = [];

    // Estimate lines to read: ~10 lines/sec → minutes * 600, cap at 5000
    const tailLines = Math.min(minutes * 600, 5000);

    for (const logPath of logFiles) {
      const output = await sshExecSafe(host, `tail -${tailLines} "${logPath}" 2>/dev/null`, 15_000);
      if (!output || !output.trim()) {
        fileResults.push({ log_path: logPath, lines_scanned: 0, matches_found: 0, stat_dump_lines_filtered: 0 });
        continue;
      }

      const lines = output.split("\n");
      let matchCount = 0;
      let statDumpFiltered = 0;
      let inStatBlock = false;

      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];

        // Track stat block boundaries
        if (line.includes("M T") && line.includes("D E V") && line.includes("S T A T E")) {
          inStatBlock = true;
          continue;
        }
        if (line.includes("E N D") && line.includes("S T A T E")) {
          inStatBlock = false;
          continue;
        }

        // Classify line
        const cls = classifyLine(line, inStatBlock, patternRe);
        if (cls === null) continue;

        // Handle stat dump lines
        if (cls === "stat_dump") {
          statDumpFiltered++;
          if (severity !== "all" || !excludeStatDumps) continue;
          // In "all" mode with exclude_stat_dumps, still count but don't add to matches
          if (excludeStatDumps) continue;
        }

        // Filter by severity
        if (severity === "error" && cls !== "error") continue;
        if (severity === "warning" && cls !== "error" && cls !== "warning") continue;

        // Parse timestamp and filter by time window
        const timestamp = parseTimestamp(line);
        if (timestamp) {
          const lineTime = new Date(timestamp + "Z").getTime();
          if (!isNaN(lineTime) && lineTime < cutoffMs) continue;
        }

        matchCount++;
        allMatches.push({
          log_path: logPath,
          line_number: i + 1,
          timestamp,
          raw_line: line.trim(),
          classification: cls,
        });
      }

      fileResults.push({
        log_path: logPath,
        lines_scanned: lines.length,
        matches_found: matchCount,
        stat_dump_lines_filtered: statDumpFiltered,
      });
    }

    // Sort by timestamp (newest first), with untimedstamped at the end
    allMatches.sort((a, b) => {
      if (a.timestamp && b.timestamp) return b.timestamp.localeCompare(a.timestamp);
      if (a.timestamp) return -1;
      if (b.timestamp) return 1;
      return 0;
    });

    // Deduplication
    let finalMatches = allMatches;
    if (dedup && allMatches.length > 0) {
      const groups = new Map<string, LogSearchMatch[]>();
      for (const match of allMatches) {
        const key = `${match.log_path}::${normalizeLine(match.raw_line)}`;
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key)!.push(match);
      }

      finalMatches = [];
      for (const [, group] of groups) {
        // Keep the most recent (first after sort)
        const representative = group[0];
        if (group.length > 1) {
          representative.dedup_count = group.length;
        }
        finalMatches.push(representative);
      }

      // Re-sort after dedup
      finalMatches.sort((a, b) => {
        if (a.timestamp && b.timestamp) return b.timestamp.localeCompare(a.timestamp);
        if (a.timestamp) return -1;
        if (b.timestamp) return 1;
        return 0;
      });
    }

    // Truncate
    const truncated = finalMatches.length > maxResults;
    if (truncated) {
      finalMatches = finalMatches.slice(0, maxResults);
    }

    return okResponse<LogSearchData>(
      {
        matches: finalMatches,
        match_count: finalMatches.length,
        truncated,
        files: fileResults,
        files_scanned: fileResults.length,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "LOG_SEARCH_ERROR",
      `Failed to search logs: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
