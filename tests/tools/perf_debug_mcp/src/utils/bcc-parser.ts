/**
 * Shared parsers for BCC (BPF Compiler Collection) tool output.
 *
 * BCC tools from bpfcc-tools output histograms, folded stacks, and
 * tabular data in well-known formats.  These parsers normalise output
 * into structured TypeScript objects reused across many tools.
 */

/** One bucket in a BCC power-of-2 histogram (e.g., runqlat, cpudist). */
export interface BccHistogramBucket {
  /** Inclusive lower bound of the bucket (µs or ms, depending on tool). */
  lo: number;
  /** Exclusive upper bound of the bucket (µs or ms). */
  hi: number;
  /** Number of events in this bucket. */
  count: number;
  /** ASCII bar string (e.g., "|@@@@@   |") — preserved for debug. */
  bar?: string;
}

/** A complete histogram block (one per-CPU or aggregate). */
export interface BccHistogram {
  /** Label for this histogram (e.g., "cpu = 5" or "all"). */
  label: string;
  /** Raw unit the tool was run with: "usecs" | "msecs". */
  unit: "usecs" | "msecs";
  /** Total events across all buckets. */
  total_count: number;
  /** Estimated average from bucket midpoints, in the histogram's unit. */
  avg_value: number;
  /** P50 estimated from buckets, in the histogram's unit. */
  p50: number;
  /** P99 estimated from buckets, in the histogram's unit. */
  p99: number;
  /** The individual buckets. */
  buckets: BccHistogramBucket[];
}

/** A folded-stack entry (e.g., offcputime, wakeuptime). */
export interface BccStackEntry {
  /** Stack frames from bottom (kernel) to top (user). */
  frames: string[];
  /** Total time or count associated with this stack. */
  value: number;
}

/* ── BCC tool binary resolution ──────────────────────────────────────────── */

/**
 * Resolve the BCC tool binary name.  On Ubuntu/Debian the tools are
 * suffixed with `-bpfcc` (e.g., `runqlat-bpfcc`), while on Fedora/RHEL
 * they live as bare names in /usr/share/bcc/tools/.
 *
 * Returns a shell snippet that resolves at runtime so a single sshExec
 * call works cross-distro.
 */
export function bccBinaryCmd(toolName: string): string {
  // Try the -bpfcc suffix first (Ubuntu/Debian), then the bare name
  // (Fedora/RHEL installs into /usr/share/bcc/tools/), then fail.
  return `{ command -v ${toolName}-bpfcc 2>/dev/null || command -v ${toolName} 2>/dev/null || echo ""; }`;
}

/**
 * Shell snippet that checks whether a BCC tool is available.
 * Returns exit 0 if found, exit 1 otherwise.
 */
export function bccAvailableCheck(toolName: string): string {
  return `command -v ${toolName}-bpfcc >/dev/null 2>&1 || command -v ${toolName} >/dev/null 2>&1`;
}

/* ── Histogram parser ────────────────────────────────────────────────────── */

/**
 * Parse BCC histogram output into structured data.
 *
 * Typical format:
 * ```
 *      usecs               : count    distribution
 *          0 -> 1          : 0        |                                        |
 *          2 -> 3          : 15       |@@@@@                                   |
 *          4 -> 7          : 100      |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
 *          8 -> 15         : 35       |@@@@@@@@@@@@                            |
 *         16 -> 31         : 5        |@@                                      |
 * ```
 *
 * Multiple histograms may be separated by labels like:
 * ```
 *      cpu = 5
 *      usecs               : count    distribution
 *      ...
 * ```
 */
export function parseBccHistograms(output: string): BccHistogram[] {
  const lines = output.split("\n");
  const histograms: BccHistogram[] = [];
  let currentLabel = "all";
  let currentUnit: "usecs" | "msecs" = "usecs";
  let currentBuckets: BccHistogramBucket[] = [];

  const flushHistogram = () => {
    if (currentBuckets.length > 0) {
      const totalCount = currentBuckets.reduce((s, b) => s + b.count, 0);
      const avgValue = totalCount > 0
        ? currentBuckets.reduce((s, b) => s + b.count * ((b.lo + b.hi) / 2), 0) / totalCount
        : 0;
      const p50 = estimatePercentile(currentBuckets, totalCount, 0.50);
      const p99 = estimatePercentile(currentBuckets, totalCount, 0.99);
      histograms.push({
        label: currentLabel,
        unit: currentUnit,
        total_count: totalCount,
        avg_value: Math.round(avgValue * 100) / 100,
        p50,
        p99,
        buckets: currentBuckets,
      });
      currentBuckets = [];
    }
  };

  // Regex patterns
  const headerRe = /^\s*(usecs|msecs)\s*:\s*count\s+distribution/;
  const bucketRe = /^\s*(\d+)\s*->\s*(\d+)\s*:\s*(\d+)\s*(\|.*\|)?/;
  const labelRe = /^\s*(\S.*\S)\s*$/;  // fallback label detection

  for (const line of lines) {
    // Header line: "usecs : count distribution"
    const headerMatch = line.match(headerRe);
    if (headerMatch) {
      // Don't flush yet — flush when we see a new label or end
      currentUnit = headerMatch[1] as "usecs" | "msecs";
      continue;
    }

    // Bucket line: "  4 -> 7          : 100      |@@@@|"
    const bucketMatch = line.match(bucketRe);
    if (bucketMatch) {
      currentBuckets.push({
        lo: parseInt(bucketMatch[1], 10),
        hi: parseInt(bucketMatch[2], 10),
        count: parseInt(bucketMatch[3], 10),
        bar: bucketMatch[4]?.trim(),
      });
      continue;
    }

    // Potential label line (e.g., "cpu = 5" or "disk = sda")
    // Must not look like a bucket or header
    const trimmed = line.trim();
    if (trimmed && !trimmed.startsWith("Tracing") && !trimmed.startsWith("@") &&
        !trimmed.startsWith("#") && !trimmed.startsWith("usecs") && !trimmed.startsWith("msecs") &&
        !trimmed.match(/^\d/) && trimmed.length < 100) {
      // This could be a label for a new histogram block
      // Flush previous if we had buckets
      flushHistogram();
      currentLabel = trimmed;
    }
  }

  // Flush last histogram
  flushHistogram();

  return histograms;
}

/**
 * Estimate a percentile from histogram buckets.
 * Uses linear interpolation within the bucket that contains the target rank.
 */
function estimatePercentile(
  buckets: BccHistogramBucket[],
  totalCount: number,
  percentile: number,
): number {
  if (totalCount === 0) return 0;
  const target = totalCount * percentile;
  let cumulative = 0;

  for (const b of buckets) {
    cumulative += b.count;
    if (cumulative >= target) {
      // The target lies in this bucket
      // Return the bucket midpoint as the estimate
      return Math.round(((b.lo + b.hi) / 2) * 100) / 100;
    }
  }
  // Fallback: return last bucket upper bound
  const last = buckets[buckets.length - 1];
  return last ? last.hi : 0;
}

/* ── Folded-stack parser ─────────────────────────────────────────────────── */

/**
 * Parse BCC folded-stack output (used by offcputime, wakeuptime, etc.)
 *
 * Format:
 * ```
 *   frame1;frame2;frame3 42
 *   frame1;frame4 100
 * ```
 *
 * Optionally supports a `topN` limit to keep only the highest-value stacks.
 */
export function parseFoldedStacks(output: string, topN?: number): BccStackEntry[] {
  const lines = output.split("\n").map((l) => l.trim()).filter(Boolean);
  const entries: BccStackEntry[] = [];

  for (const line of lines) {
    // Skip tracing/header lines
    if (line.startsWith("Tracing") || line.startsWith("#") || line.startsWith("@")) continue;

    // Format: "frame1;frame2;frame3 12345" — value is the last whitespace-delimited token
    const lastSpaceIdx = line.lastIndexOf(" ");
    if (lastSpaceIdx < 0) continue;

    const stackPart = line.slice(0, lastSpaceIdx).trim();
    const valuePart = line.slice(lastSpaceIdx + 1).trim();
    const value = parseInt(valuePart, 10);
    if (isNaN(value) || !stackPart) continue;

    const frames = stackPart.split(";").filter(Boolean);
    entries.push({ frames, value });
  }

  // Sort by value descending
  entries.sort((a, b) => b.value - a.value);

  if (topN !== undefined && topN > 0) {
    return entries.slice(0, topN);
  }
  return entries;
}

/* ── Tabular BCC output parser ───────────────────────────────────────────── */

/**
 * Parse tabular BCC output (e.g., llcstat, hardirqs count mode).
 *
 * Format:
 * ```
 * FUNC                              COUNT
 * __do_softirq                      12345
 * ```
 */
export function parseBccTable(output: string): Array<Record<string, string>> {
  const lines = output.split("\n").filter((l) => l.trim().length > 0);
  if (lines.length < 2) return [];

  // Skip any "Tracing..." preamble lines
  let headerIdx = -1;
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (trimmed.startsWith("Tracing") || trimmed.startsWith("#")) continue;
    // First line with multiple uppercase words is likely the header
    headerIdx = i;
    break;
  }
  if (headerIdx < 0) return [];

  // Parse header: split on 2+ spaces
  const headerLine = lines[headerIdx];
  const headers = headerLine.trim().split(/\s{2,}/).map((h) => h.trim());
  if (headers.length === 0) return [];

  const rows: Array<Record<string, string>> = [];
  for (let i = headerIdx + 1; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    if (!trimmed || trimmed.startsWith("Detaching") || trimmed.startsWith("Tracing")) continue;

    // Split on 2+ spaces, aligning with headers
    const parts = trimmed.split(/\s{2,}/).map((p) => p.trim());
    if (parts.length === 0) continue;

    const row: Record<string, string> = {};
    for (let j = 0; j < headers.length; j++) {
      row[headers[j]] = parts[j] ?? "";
    }
    rows.push(row);
  }

  return rows;
}
