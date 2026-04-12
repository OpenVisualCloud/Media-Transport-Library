/**
 * Utility helpers for reading proc/sys files.
 */
import { readFile, readdir, access, constants } from "fs/promises";

/**
 * Read a file, return its content or null on error.
 */
export async function readFileOr(path: string, fallback: string | null = null): Promise<string | null> {
  try {
    return await readFile(path, "utf-8");
  } catch {
    return fallback;
  }
}

/**
 * Read a file and return trimmed content.
 */
export async function readFileTrimmed(path: string): Promise<string | null> {
  const content = await readFileOr(path);
  return content?.trim() ?? null;
}

/**
 * Read an integer value from a file.
 */
export async function readFileInt(path: string): Promise<number | null> {
  const raw = await readFileTrimmed(path);
  if (raw === null) return null;
  const val = parseInt(raw, 10);
  return isNaN(val) ? null : val;
}

/**
 * Check if a path exists and is readable.
 */
export async function isReadable(path: string): Promise<boolean> {
  try {
    await access(path, constants.R_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * List directories matching a pattern in a parent dir.
 */
export async function listDirs(parent: string): Promise<string[]> {
  try {
    const entries = await readdir(parent, { withFileTypes: true });
    return entries.filter((e) => e.isDirectory()).map((e) => e.name);
  } catch {
    return [];
  }
}

/**
 * List all entries (files + dirs) in a directory.
 */
export async function listEntries(parent: string): Promise<string[]> {
  try {
    return await readdir(parent);
  } catch {
    return [];
  }
}

/**
 * Parse a CPU list string like "0-3,5,7-9" into an array of CPU numbers.
 */
export function parseCpuList(list: string): number[] {
  const cpus: number[] = [];
  const trimmed = list.trim();
  if (!trimmed || trimmed === "") return cpus;
  for (const part of trimmed.split(",")) {
    const p = part.trim();
    if (p === "") continue;
    const range = p.split("-");
    if (range.length === 2) {
      const start = parseInt(range[0], 10);
      const end = parseInt(range[1], 10);
      if (!isNaN(start) && !isNaN(end)) {
        for (let i = start; i <= end; i++) cpus.push(i);
      }
    } else {
      const val = parseInt(p, 10);
      if (!isNaN(val)) cpus.push(val);
    }
  }
  return cpus.sort((a, b) => a - b);
}

/**
 * Parse a hex mask string (e.g., "ff" or "00000000,ff000000") into CPU list.
 */
export function parseHexMask(mask: string): number[] {
  const cpus: number[] = [];
  // Remove commas (used for 32-bit grouping) and leading zeros
  const hex = mask.replace(/,/g, "").trim();
  const bigVal = BigInt("0x" + hex);
  for (let i = 0; i < 1024; i++) {
    if (bigVal & (1n << BigInt(i))) {
      cpus.push(i);
    }
  }
  return cpus;
}

/**
 * Convert a CPU list to a hex mask string.
 */
export function cpuListToHexMask(cpus: number[]): string {
  let val = 0n;
  for (const c of cpus) {
    val |= 1n << BigInt(c);
  }
  return val.toString(16);
}

/**
 * Sleep for a specified number of milliseconds.
 */
export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * Get current wall clock time in ms (high resolution).
 */
export function nowMs(): number {
  return performance.now();
}
