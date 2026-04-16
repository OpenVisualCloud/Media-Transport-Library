/**
 * emon_capabilities() — Return EMON availability, version, drivers, PMU types, and available presets.
 *
 * This is the first tool to call when working with EMON. It tells you:
 *   - Whether EMON is installed and drivers are loaded
 *   - EMON version and how many events are supported
 *   - Available PMU types (core, cha, iio, upi, imc, etc.)
 *   - Available presets and their descriptions
 */
import type { ToolResponse } from "../types.js";
import type { EmonConnectionStatus, EmonPresetInfo } from "../types-emon.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { getEmonBridge, EMON_PRESETS } from "../collectors/emon-bridge.js";

export interface EmonCapabilitiesData {
  connection: EmonConnectionStatus;
  presets: EmonPresetInfo[];
}

export async function emonCapabilities(): Promise<ToolResponse<EmonCapabilitiesData>> {
  const meta = await buildMeta("fallback");

  try {
    const bridge = getEmonBridge();

    // If EMON binary exists but drivers not loaded, try to load them
    if (!bridge.isAvailable) {
      const { autoLoadMessage } = await bridge.ensureReady();
      if (autoLoadMessage) {
        // Include the auto-load attempt result in the status
        const status = bridge.getStatus();
        return okResponse(
          {
            connection: { ...status, auto_load_message: autoLoadMessage },
            presets: Object.values(EMON_PRESETS),
          },
          meta,
        );
      }
    }

    const status = bridge.getStatus();

    return okResponse(
      {
        connection: status,
        presets: Object.values(EMON_PRESETS),
      },
      meta,
    );
  } catch (err: any) {
    return errorResponse(
      meta,
      "EMON_CAPABILITIES_ERROR",
      `Failed to gather EMON capabilities: ${err?.message ?? err}`,
    );
  }
}
