/**
 * Host-aware file I/O utilities.
 *
 * Provides drop-in replacements for local proc/sys readers that
 * transparently work on remote hosts via SSH.
 */
import { readFile, readdir, access, constants } from "fs/promises";
import { sshExecOrThrow, sshExecSafe } from "../utils/ssh-exec.js";

/**
 * Returns true when the host string means "this machine".
 */
export function isLocalHost(host?: string): boolean {
  return !host || host === "localhost" || host === "127.0.0.1" || host === "";
}

/**
 * Validate a path contains only safe characters (no shell injection).
 * Proc/sys paths are always alphanumeric with slashes, dots, dashes, underscores.
 */
function assertSafePath(path: string): void {
  if (!/^[a-zA-Z0-9_.\/\-]+$/.test(path)) {
    throw new Error(`Unsafe path rejected: ${path}`);
  }
}

// ── Single-file readers ─────────────────────────────────────────────

/**
 * Read a file — locally or via SSH.
 */
export async function readFileHost(host: string | undefined, path: string): Promise<string> {
  if (isLocalHost(host)) {
    return readFile(path, "utf-8");
  }
  assertSafePath(path);
  return sshExecOrThrow(host!, `cat '${path}'`);
}

/**
 * Read a file, return null on error.
 */
export async function readFileOrHost(
  host: string | undefined,
  path: string,
): Promise<string | null> {
  try {
    return await readFileHost(host, path);
  } catch {
    return null;
  }
}

/**
 * Read a file, trim result, return null on error.
 */
export async function readFileTrimmedHost(
  host: string | undefined,
  path: string,
): Promise<string | null> {
  const content = await readFileOrHost(host, path);
  return content?.trim() ?? null;
}

/**
 * Read an integer from a file.
 */
export async function readFileIntHost(
  host: string | undefined,
  path: string,
): Promise<number | null> {
  const raw = await readFileTrimmedHost(host, path);
  if (raw === null) return null;
  const val = parseInt(raw, 10);
  return isNaN(val) ? null : val;
}

// ── Directory listing ───────────────────────────────────────────────

/**
 * List entries in a directory — locally or via SSH.
 */
export async function listEntriesHost(
  host: string | undefined,
  path: string,
): Promise<string[]> {
  if (isLocalHost(host)) {
    try {
      return await readdir(path);
    } catch {
      return [];
    }
  }
  assertSafePath(path);
  const result = await sshExecSafe(host!, `ls '${path}' 2>/dev/null`);
  if (!result) return [];
  return result.trim().split("\n").filter(Boolean);
}

/**
 * List directories (entries that are directories) inside a parent.
 */
export async function listDirsHost(
  host: string | undefined,
  parent: string,
): Promise<string[]> {
  if (isLocalHost(host)) {
    try {
      const entries = await readdir(parent, { withFileTypes: true });
      return entries.filter((e) => e.isDirectory()).map((e) => e.name);
    } catch {
      return [];
    }
  }
  assertSafePath(parent);
  const result = await sshExecSafe(
    host!,
    `find '${parent}' -maxdepth 1 -mindepth 1 -type d -printf '%f\\n' 2>/dev/null`,
  );
  if (!result) return [];
  return result.trim().split("\n").filter(Boolean);
}

/**
 * Check if a path is readable.
 */
export async function isReadableHost(
  host: string | undefined,
  path: string,
): Promise<boolean> {
  if (isLocalHost(host)) {
    try {
      await access(path, constants.R_OK);
      return true;
    } catch {
      return false;
    }
  }
  assertSafePath(path);
  const result = await sshExecSafe(host!, `test -r '${path}' && echo yes`);
  return result?.trim() === "yes";
}

// ── Batch file readers (single SSH round-trip) ──────────────────────

const BATCH_DELIM = "===CPU_DBG_FILE===";

/**
 * Read multiple files in a single SSH call.
 * Returns a Map of path → content (missing files get null).
 */
export async function readFilesHost(
  host: string | undefined,
  paths: string[],
): Promise<Map<string, string | null>> {
  if (isLocalHost(host)) {
    // Local: just read sequentially (fast enough for in-memory filesystems)
    const result = new Map<string, string | null>();
    for (const p of paths) {
      try {
        result.set(p, await readFile(p, "utf-8"));
      } catch {
        result.set(p, null);
      }
    }
    return result;
  }

  for (const p of paths) assertSafePath(p);

  // Build a shell script that reads all files with delimiters
  const script = paths
    .map((p) => `echo '${BATCH_DELIM} ${p}'; cat '${p}' 2>/dev/null || echo '${BATCH_DELIM} ERROR'`)
    .join("; ");

  const stdout = await sshExecOrThrow(host!, script, 30_000);
  return parseBatchReadOutput(paths, stdout);
}

function parseBatchReadOutput(
  paths: string[],
  stdout: string,
): Map<string, string | null> {
  const result = new Map<string, string | null>();
  const lines = stdout.split("\n");
  let currentPath: string | null = null;
  let currentLines: string[] = [];

  const flushCurrent = () => {
    if (currentPath !== null) {
      const content = currentLines.join("\n");
      if (content.trim() === `${BATCH_DELIM} ERROR`) {
        result.set(currentPath, null);
      } else {
        result.set(currentPath, content);
      }
    }
  };

  for (const line of lines) {
    if (line.startsWith(BATCH_DELIM)) {
      flushCurrent();
      const rest = line.substring(BATCH_DELIM.length).trim();
      if (rest === "ERROR") {
        // Belongs to current section — mark as error
        if (currentPath !== null) {
          result.set(currentPath, null);
          currentPath = null;
          currentLines = [];
        }
      } else {
        currentPath = rest;
        currentLines = [];
      }
    } else {
      currentLines.push(line);
    }
  }
  flushCurrent();

  // Ensure all requested paths are in the result
  for (const p of paths) {
    if (!result.has(p)) result.set(p, null);
  }

  return result;
}
