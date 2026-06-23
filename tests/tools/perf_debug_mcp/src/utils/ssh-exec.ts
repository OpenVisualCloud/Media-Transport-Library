/**
 * SSH command execution utility.
 *
 * All MTL tools operate on potentially remote hosts via SSH.
 * When host is "localhost" or "127.0.0.1", commands run locally.
 * Otherwise, commands are executed via `ssh root@<host> '<command>'`.
 *
 * The SSH key must already be set up (no password prompts).
 */
import { execFile } from "child_process";
import { promisify } from "util";

const execFileAsync = promisify(execFile);

/** Maximum command execution time (ms) */
const DEFAULT_TIMEOUT = 15_000;

export interface ExecResult {
  stdout: string;
  stderr: string;
  exitCode: number;
}

/**
 * Execute a shell command, either locally or via SSH on a remote host.
 *
 * @param host - Target hostname/IP.  "localhost" or "127.0.0.1" runs locally.
 * @param command - The shell command string to execute.
 * @param timeoutMs - Max execution time (default 15s).
 */
export async function sshExec(
  host: string,
  command: string,
  timeoutMs: number = DEFAULT_TIMEOUT,
): Promise<ExecResult> {
  const isLocal = host === "localhost" || host === "127.0.0.1" || host === "";

  try {
    let result: { stdout: string; stderr: string };
    if (isLocal) {
      result = await execFileAsync("bash", ["-c", command], {
        timeout: timeoutMs,
        maxBuffer: 4 * 1024 * 1024,   // 4 MB
      });
    } else {
      result = await execFileAsync(
        "ssh",
        [
          "-o", "StrictHostKeyChecking=no",
          "-o", "ConnectTimeout=5",
          "-o", "BatchMode=yes",
          `root@${host}`,
          command,
        ],
        {
          timeout: timeoutMs,
          maxBuffer: 4 * 1024 * 1024,
        },
      );
    }
    return { stdout: result.stdout, stderr: result.stderr, exitCode: 0 };
  } catch (err: unknown) {
    // execFile throws on non-zero exit code too — capture what we can
    const e = err as { stdout?: string; stderr?: string; code?: number | string; killed?: boolean };
    if (e.killed) {
      return { stdout: e.stdout ?? "", stderr: "Command timed out", exitCode: 124 };
    }
    return {
      stdout: e.stdout ?? "",
      stderr: e.stderr ?? String(err),
      exitCode: typeof e.code === "number" ? e.code : 1,
    };
  }
}

/**
 * Execute a command and return only stdout (trimmed).
 * Throws on non-zero exit or timeout.
 */
export async function sshExecOrThrow(
  host: string,
  command: string,
  timeoutMs?: number,
): Promise<string> {
  const result = await sshExec(host, command, timeoutMs);
  if (result.exitCode !== 0) {
    throw new Error(`Command failed (exit ${result.exitCode}): ${result.stderr.slice(0, 500)}`);
  }
  return result.stdout;
}

/**
 * Execute a command and return stdout, or null on any failure.
 */
export async function sshExecSafe(
  host: string,
  command: string,
  timeoutMs?: number,
): Promise<string | null> {
  const result = await sshExec(host, command, timeoutMs);
  if (result.exitCode !== 0) return null;
  return result.stdout;
}
