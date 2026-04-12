/**
 * pcm_accel — Intel accelerator utilization monitoring via pcm-accel CLI.
 *
 * Monitors IAA (In-Memory Analytics Accelerator), DSA (Data Streaming Accelerator),
 * or QAT (QuickAssist Technology) on Intel Xeon Scalable 4th Gen+ (Sapphire Rapids+).
 *
 * Metrics per device:
 *   - Inbound/Outbound bandwidth (bytes/sec)
 *   - Shared/Dedicated work queue request counts
 *
 * Requires: pcm-accel binary in PATH, root access, appropriate accelerator driver loaded.
 */
import type { ToolResponse, PcmAccelData, PcmAccelEntry } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExec } from "../utils/ssh-exec.js";

function parseAccelCsv(raw: string, target: "iaa" | "dsa" | "qat"): PcmAccelEntry[] {
  const devices: PcmAccelEntry[] = [];

  // CSV format:
  // Accelerator,Socket,Inbound_BW(Bps),Outbound_BW(Bps),ShareWQ_ReqNb,DedicateWQ_ReqNb
  // IAA#0,0,0,0,0,0
  const lines = raw.split("\n");
  let headerFound = false;

  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed.startsWith("Accelerator,")) {
      headerFound = true;
      continue;
    }
    if (!headerFound) continue;
    if (!trimmed || trimmed.startsWith("Cleaning") || trimmed.startsWith(" ")) break;

    const parts = trimmed.split(",");
    if (parts.length < 6) continue;

    const accelName = parts[0].trim();
    if (!accelName) continue;

    devices.push({
      accelerator: accelName,
      socket: parseInt(parts[1], 10) || 0,
      inbound_bw_bps: parseFloat(parts[2]) || 0,
      outbound_bw_bps: parseFloat(parts[3]) || 0,
      share_wq_req_nb: parseInt(parts[4], 10) || 0,
      dedicate_wq_req_nb: parseInt(parts[5], 10) || 0,
    });
  }

  return devices;
}

export async function pcmAccel(params: {
  target?: "iaa" | "dsa" | "qat";
  host?: string;
}): Promise<ToolResponse<PcmAccelData>> {
  const host = params.host ?? "localhost";
  const target = params.target ?? "iaa";

  const meta = await buildMeta("fallback");

  const cmd = `pcm-accel -${target} -csv -i=1 2>&1`;
  const result = await sshExec(host, cmd, 20_000);
  const raw = result.stdout;

  if (raw.includes("not found") || raw.includes("No such file")) {
    return errorResponse(
      meta,
      "PCM_NOT_FOUND",
      "pcm-accel binary not found",
      "Install Intel PCM: https://github.com/intel/pcm"
    );
  }

  if (raw.includes("driver") && raw.includes("fail")) {
    return errorResponse(
      meta,
      "PCM_DRIVER_MISSING",
      `Accelerator driver for ${target.toUpperCase()} may not be loaded`,
      `Ensure the ${target} driver (e.g., idxd for IAA/DSA, qat for QAT) is loaded`
    );
  }

  const devices = parseAccelCsv(raw, target);

  return okResponse({ target, devices }, meta);
}
