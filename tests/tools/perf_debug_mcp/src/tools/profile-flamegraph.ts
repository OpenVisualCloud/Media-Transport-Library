/**
 * profile_flamegraph — CPU profiling with folded-stack output for flame graphs.
 *
 * Samples CPU stacks at a given frequency and produces folded-stack output
 * suitable for direct input to Brendan Gregg's flamegraph.pl or speedscope.
 *
 * The raw folded output is included so the LLM can directly generate flame
 * graphs.  Top stacks are also returned as structured data.
 *
 * Source: BCC `profile` (uses perf_event sampling).
 * Requires: bpfcc-tools, root/CAP_BPF+CAP_PERFMON.
 */
import type { ToolResponse, ProfileFlamegraphData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";
import { bccBinaryCmd, parseFoldedStacks } from "../utils/bcc-parser.js";

export async function profileFlamegraph(params: {
  host?: string;
  duration_sec?: number;
  frequency_hz?: number;
  pid?: number;
  user_only?: boolean;
  kernel_only?: boolean;
}): Promise<ToolResponse<ProfileFlamegraphData>> {
  const host = params.host ?? "localhost";
  const duration = params.duration_sec ?? 5;
  const frequency = params.frequency_hz ?? 49;
  const pid = params.pid;
  const userOnly = params.user_only ?? false;
  const kernelOnly = params.kernel_only ?? false;

  const meta = await buildMeta("fallback", duration * 1000);

  const binCmd = bccBinaryCmd("profile");
  const bin = await sshExecSafe(host, binCmd, 5_000);
  if (!bin || !bin.trim()) {
    return errorResponse(
      meta,
      "PROFILE_MISSING",
      "profile (BCC) not found on target host",
      "Install bpfcc-tools: apt-get install bpfcc-tools",
    );
  }
  const binary = bin.trim();

  const flags: string[] = ["-f", "-F", String(frequency)]; // folded output
  if (pid !== undefined) flags.push(`-p ${pid}`);
  if (userOnly) flags.push("-U");
  if (kernelOnly) flags.push("-K");
  const cmd = `${binary} ${flags.join(" ")} ${duration} 2>/dev/null`;

  const timeoutMs = (duration + 15) * 1000;
  const output = await sshExecSafe(host, cmd, timeoutMs);
  if (!output || !output.trim()) {
    return errorResponse(
      meta,
      "PROFILE_NO_OUTPUT",
      "profile produced no output",
      "Ensure root/CAP_BPF+CAP_PERFMON. Verify target has running threads.",
    );
  }

  try {
    const allStacks = parseFoldedStacks(output);
    const totalSamples = allStacks.reduce((s, e) => s + e.value, 0);

    // Return the raw folded output (trimmed to reasonable size)
    const foldedStacks = output.trim().slice(0, 500_000);

    const topStacks = allStacks.slice(0, 30).map((s) => ({
      frames: s.frames,
      count: s.value,
    }));

    const warnings: string[] = [];
    if (totalSamples < 10) {
      warnings.push(
        `Only ${totalSamples} samples captured — consider increasing duration or frequency`,
      );
    }

    return okResponse<ProfileFlamegraphData>({
      duration_sec: duration,
      frequency_hz: frequency,
      pid_filter: pid,
      folded_stacks: foldedStacks,
      total_samples: totalSamples,
      top_stacks: topStacks,
      warnings,
    }, meta);
  } catch (err) {
    return errorResponse(
      meta,
      "PROFILE_PARSE_ERROR",
      `Failed to parse profile output: ${err instanceof Error ? err.message : String(err)}`,
    );
  }
}
