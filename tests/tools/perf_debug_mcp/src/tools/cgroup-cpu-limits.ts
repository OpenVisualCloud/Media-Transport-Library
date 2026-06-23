/**
 * cgroup_cpu_limits(target_pid) — Show CPU quotas/cpuset restrictions.
 */
import type { ToolResponse, CgroupCpuLimitsData } from "../types.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { readCgroupCpuLimits } from "../collectors/cgroup.js";

export async function cgroupCpuLimits(params: {
  target_pid: number;
  host?: string;
}): Promise<ToolResponse<CgroupCpuLimitsData>> {
  const meta = await buildMeta("fallback");

  try {
    const data = await readCgroupCpuLimits(params.target_pid, params.host);
    if (!data) {
      return errorResponse(
        meta,
        "CGROUP_NOT_FOUND",
        `No cgroup v2 data found for PID ${params.target_pid}`,
        "This process may not be in a cgroup, or cgroup v2 may not be mounted"
      );
    }

    return okResponse(data, meta);
  } catch (err) {
    return errorResponse(meta, "CGROUP_CPU_LIMITS_ERROR", `Failed: ${err}`);
  }
}
