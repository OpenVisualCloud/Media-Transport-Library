/**
 * mtl-usdt-probes.ts — List available USDT probes in libmtl.so
 *
 * Enumerates all USDT probes grouped by provider.  If a PID is specified,
 * also verifies which probes are resolvable for that running process.
 */
import { z } from "zod";
import { getBpftraceBridge } from "../collectors/bpftrace-bridge.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import type { UsdtProbeListData, UsdtProbesByProvider } from "../types-mtl.js";

export const mtlUsdtProbesSchema = z.object({
  pid: z.number().optional().describe("Optional PID of a running MTL process to verify probe resolution against"),
});

export async function mtlUsdtProbes(params: z.infer<typeof mtlUsdtProbesSchema>) {
  const bridge = getBpftraceBridge();
  const meta = await buildMeta("usdt");

  if (!bridge.isAvailable) {
    return errorResponse(meta, "BPFTRACE_UNAVAILABLE",
      "bpftrace is not available", "Run setup.sh or install bpftrace");
  }

  if (!bridge.libmtlPath) {
    return errorResponse(meta, "LIBMTL_NOT_FOUND",
      "libmtl.so not found — MTL not installed with USDT support",
      "Build MTL with -Denable_usdt=true and install system-wide");
  }

  const probes = await bridge.listProbes();
  if (probes.length === 0) {
    return errorResponse(meta, "NO_PROBES",
      "No USDT probes found in " + bridge.libmtlPath,
      "Ensure MTL was built with USDT support (systemtap-sdt-dev must be installed at build time)");
  }

  // Group by provider
  const providerMap = new Map<string, string[]>();
  for (const p of probes) {
    if (!providerMap.has(p.provider)) providerMap.set(p.provider, []);
    providerMap.get(p.provider)!.push(p.probe);
  }

  const providers: UsdtProbesByProvider[] = [];
  for (const [provider, probeNames] of providerMap) {
    providers.push({ provider, probes: probeNames.sort(), count: probeNames.length });
  }
  providers.sort((a, b) => a.provider.localeCompare(b.provider));

  const data: UsdtProbeListData = {
    libmtl_path: bridge.libmtlPath,
    total_probes: probes.length,
    providers,
    ...(params.pid != null ? { pid: params.pid } : {}),
  };

  return okResponse(data, meta);
}
