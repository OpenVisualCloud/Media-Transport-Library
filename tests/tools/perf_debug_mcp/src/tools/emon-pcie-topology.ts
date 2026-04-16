/**
 * emon_pcie_topology() — Map IIO stacks to physical PCIe devices.
 *
 * Reads /sys/bus/pci/devices/* and /sys/class/net/* to build a mapping
 * from IIO (socket:stack:part) identifiers back to physical PCIe devices
 * (BDF, driver, NUMA node, network interface name).
 *
 * This is essential for interpreting E0 (IIO PCIe per-port) data:
 * when EMON tells you "socket 0, stack 7, part 0 has 2 GB/s traffic",
 * this tool tells you that's "ens1np0 (Intel E810-C, ice driver)".
 *
 * IIO stack mapping is discovered via pcm-iio --list, which reads
 * the silicon's MMIO discovery table to produce the authoritative mapping
 * from PCI root bus to IIO PMU unit.  On SPR the ordering is NOT
 * sequential by bus number — it follows the die fabric layout.
 * Falls back to a simple heuristic if pcm-iio is unavailable.
 */
import { readFile, readdir, readlink, realpath } from "fs/promises";
import { basename } from "path";
import { execFile } from "child_process";
import { promisify } from "util";
import type { ToolResponse } from "../types.js";
import type { EmonPcieTopologyData, PcieDeviceMapping } from "../types-emon.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";

const execFileAsync = promisify(execFile);

// ─── IIO stack discovery ─────────────────────────────────────────────────────

/**
 * Map from PCI root bus (hex string) to { socket, iioPmu } by parsing
 * pcm-iio --list output.  The authoritative lines look like:
 *   "Mapped CPU bus #4 (domain 0 bus 26) to IO PMU #7 package 0"
 */
async function discoverIioMap(): Promise<Map<number, { socket: number; iioStack: number }>> {
  const map = new Map<number, { socket: number; iioStack: number }>();

  try {
    const { stdout, stderr } = await execFileAsync("pcm-iio", ["--list"], {
      timeout: 15_000,
      env: { ...process.env },
    });
    const text = stdout + "\n" + stderr;
    //  "Mapped CPU bus #4 (domain 0 bus 26) to IO PMU #7 package 0"
    const re = /Mapped CPU bus #\d+ \(domain \d+ bus ([0-9a-f]+)\) to IO PMU #(\d+) package (\d+)/gi;
    let m: RegExpExecArray | null;
    while ((m = re.exec(text)) !== null) {
      const rootBus = parseInt(m[1], 16);
      const iioStack = parseInt(m[2], 10);
      const socket = parseInt(m[3], 10);
      map.set(rootBus, { socket, iioStack });
    }
  } catch {
    // pcm-iio not available — fall back
  }

  return map;
}

/**
 * Cache the IIO map (static per boot, no need to re-discover).
 */
let cachedIioMap: Map<number, { socket: number; iioStack: number }> | null = null;

async function getIioMap(): Promise<Map<number, { socket: number; iioStack: number }>> {
  if (!cachedIioMap) {
    cachedIioMap = await discoverIioMap();
  }
  return cachedIioMap;
}

// ─── helpers ─────────────────────────────────────────────────────────────────

async function readSafe(path: string): Promise<string> {
  try {
    return (await readFile(path, "utf-8")).trim();
  } catch {
    return "";
  }
}

async function readNum(path: string): Promise<number> {
  const v = await readSafe(path);
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? n : -1;
}

/**
 * Get the IIO stack number for a given PCI device.
 *
 * Strategy:
 *   1. Walk up sysfs to find the root complex bus  (pciDDDD:XX)
 *   2. Look up that root bus in the pcm-iio derived map
 *   3. Fall back to a best-effort heuristic if pcm-iio is absent
 */
async function getIioStackForDevice(
  bdf: string,
  iioMap: Map<number, { socket: number; iioStack: number }>,
): Promise<{ socket: number; stack: number; part: number }> {
  const numaNode = await readNum(`/sys/bus/pci/devices/${bdf}/numa_node`);
  const socket = numaNode >= 0 ? numaNode : 0;

  // Parse bus from BDF (DDDD:BB:DD.F)
  const parts = bdf.split(":");
  const bus = parseInt(parts[1], 16);

  // Find the root complex bus by walking sysfs
  let rootBus = bus;
  try {
    const devicePath = await realpath(`/sys/bus/pci/devices/${bdf}`);
    // devicePath looks like /sys/devices/pci0000:YY/0000:YY:ZZ.W/.../BBBB:BB:DD.F
    const pathParts = devicePath.split("/");
    for (const p of pathParts) {
      const m = p.match(/^pci([0-9a-f]{4}):([0-9a-f]{2})$/i);
      if (m) {
        rootBus = parseInt(m[2], 16);
        break;
      }
    }
  } catch {
    // Fall through
  }

  // Look up in the pcm-iio discovered map (authoritative)
  const mapped = iioMap.get(rootBus);
  if (mapped) {
    return { socket: mapped.socket, stack: mapped.iioStack, part: 0 };
  }

  // Fallback heuristic: count unique root ports per socket in sorted order
  // and assign stack indices sequentially.  Not reliable on all platforms.
  const stack = Math.floor(rootBus / 16) % 12;
  return { socket, stack, part: 0 };
}

export async function emonPcieTopology(): Promise<ToolResponse<EmonPcieTopologyData>> {
  const meta = await buildMeta("fallback");

  try {
    const devices: PcieDeviceMapping[] = [];
    const iioMap: Record<string, PcieDeviceMapping> = {};

    // Discover IIO stack mapping via pcm-iio
    const iioStackMap = await getIioMap();

    // Build net interface → BDF map
    const netIfaceMap = new Map<string, string>(); // BDF → iface name
    try {
      const netDir = "/sys/class/net";
      const ifaces = await readdir(netDir);
      for (const iface of ifaces) {
        try {
          const devLink = await realpath(`${netDir}/${iface}/device`);
          const bdf = basename(devLink);
          if (bdf.match(/^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]$/i)) {
            netIfaceMap.set(bdf, iface);
          }
        } catch {
          // Not a PCI-backed interface — skip
        }
      }
    } catch { /* no /sys/class/net */ }

    // Scan PCI devices
    const pciDevicesDir = "/sys/bus/pci/devices";
    let pciDevices: string[];
    try {
      pciDevices = await readdir(pciDevicesDir);
    } catch {
      return errorResponse(meta, "SYSFS_ERROR", "Cannot read /sys/bus/pci/devices");
    }

    for (const bdf of pciDevices) {
      const classCode = await readSafe(`${pciDevicesDir}/${bdf}/class`);

      // Filter to interesting device classes:
      // 0x02xxxx = Network controller
      // 0x01xxxx = Storage controller
      // 0x03xxxx = Display controller
      // 0x0cxxxx = Serial bus (USB, etc.)
      // 0x12xxxx = Processing accelerator
      const classPrefix = classCode.substring(0, 4);
      if (!["0x02", "0x01", "0x03", "0x0c", "0x12", "0x06"].includes(classPrefix)) {
        continue;
      }

      // Skip bridges (0x0604 = PCI bridge)
      if (classCode.startsWith("0x0604")) continue;

      const numaNode = await readNum(`${pciDevicesDir}/${bdf}/numa_node`);

      // Get driver name
      let driver = "";
      try {
        const driverLink = await readlink(`${pciDevicesDir}/${bdf}/driver`);
        driver = basename(driverLink);
      } catch { /* no driver bound */ }

      // Get device name from vendor/device IDs (or use a simple lookup)
      const vendorId = await readSafe(`${pciDevicesDir}/${bdf}/vendor`);
      const deviceId = await readSafe(`${pciDevicesDir}/${bdf}/device`);
      const deviceName = getDeviceName(vendorId, deviceId);

      // Map to IIO stack
      const { socket, stack, part } = await getIioStackForDevice(bdf, iioStackMap);

      const dev: PcieDeviceMapping = {
        bdf,
        device_name: deviceName,
        driver,
        numa_node: numaNode,
        socket,
        iio_stack: stack,
        iio_part: part,
        net_interface: netIfaceMap.get(bdf),
      };

      devices.push(dev);
      iioMap[`${socket}:${stack}:${part}`] = dev;
    }

    // Sort by socket, stack, part
    devices.sort((a, b) =>
      a.socket !== b.socket ? a.socket - b.socket :
      a.iio_stack !== b.iio_stack ? a.iio_stack - b.iio_stack :
      a.iio_part - b.iio_part
    );

    return okResponse({ devices, iio_map: iioMap }, meta);
  } catch (err: any) {
    return errorResponse(
      meta,
      "PCIE_TOPOLOGY_ERROR",
      `Failed to enumerate PCIe topology: ${err?.message ?? err}`,
    );
  }
}

// ─── Simple device name lookup ──────────────────────────────────────────────

function getDeviceName(vendorId: string, deviceId: string): string {
  // Common Intel NIC and accelerator IDs
  const known: Record<string, string> = {
    "0x8086:0x1592": "Intel E810-C (ice)",
    "0x8086:0x1593": "Intel E810-XXV (ice)",
    "0x8086:0x159b": "Intel E810-C QSFP (ice)",
    "0x8086:0x1889": "Intel E810 VF (iavf)",
    "0x8086:0x0b60": "Intel Data Streaming Accelerator (DSA)",
    "0x8086:0x0b25": "Intel I/O Acceleration Technology (IAA)",
    "0x8086:0x0db5": "Intel QuickAssist (QAT)",
    "0x8086:0x09a2": "Intel Xeon PCIe Root Port",
    "0x8086:0x09a4": "Intel Xeon PCIe Root Port",
    "0x8086:0x3456": "Intel E810-2CQDA2",
  };

  const key = `${vendorId}:${deviceId}`;
  return known[key] ?? `PCI ${vendorId}:${deviceId}`;
}
