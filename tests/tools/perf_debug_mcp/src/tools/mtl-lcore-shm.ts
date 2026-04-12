/**
 * mtl_lcore_shm — Read MTL's SysV shared memory lcore allocation bitmap.
 *
 * MTL uses SysV SHM (key from ftok("/dev/null", 21)) to track lcore usage
 * across instances.  Each entry is 176 bytes:
 *   hostname[64] + user[32] + comm[64] + pad(4) + pid(4) + pad(4) + active(4)
 *
 * IMPORTANT: When MtlManager is running, instances may not write to SHM directly
 * (they coordinate via MtlManager Unix socket instead).  So SHM data may be
 * stale when MtlManager is active.
 *
 * Parameters:
 *   - host: target hostname/IP
 *   - ftok_path: path used for ftok (default /dev/null)
 *   - ftok_proj_id: project ID for ftok (default 21)
 */
import type { ToolResponse } from "../types.js";
import type { MtlLcoreShmData, MtlLcoreShmEntry } from "../types-mtl.js";
import { buildMeta, okResponse, errorResponse } from "../meta.js";
import { sshExecSafe } from "../utils/ssh-exec.js";

export async function mtlLcoreShm(params: {
  host?: string;
  ftok_path?: string;
  ftok_proj_id?: number;
}): Promise<ToolResponse<MtlLcoreShmData>> {
  const host = params.host ?? "localhost";
  const ftokPath = params.ftok_path ?? "/dev/null";
  const ftokProjId = params.ftok_proj_id ?? 21;

  const meta = await buildMeta("fallback");

  try {
    // Use Python with ctypes to read SHM (no sysv_ipc dependency needed)
    const script = `python3 - <<'__SHM_EOF__'
import ctypes, ctypes.util, json, struct, os

# ftok implementation matching glibc
def py_ftok(path, proj_id):
    st = os.stat(path)
    return (((proj_id & 0xFF) << 24) | ((st.st_dev & 0xFF) << 16) | (st.st_ino & 0xFFFF))

IPC_CREAT = 0o1000
SHM_RDONLY = 0o10000

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
libc.shmat.restype = ctypes.c_void_p
libc.shmat.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
libc.shmdt.argtypes = [ctypes.c_void_p]
libc.shmget.argtypes = [ctypes.c_int, ctypes.c_size_t, ctypes.c_int]

key = py_ftok(${JSON.stringify(ftokPath)}, ${ftokProjId})
shmid = libc.shmget(key, 0, 0)
if shmid < 0:
    print(json.dumps({"error": "SHM not found (no MTL SHM segment)", "key": hex(key)}))
else:
    # Get info via ipcs
    import subprocess
    try:
        info_out = subprocess.check_output(["ipcs", "-m", "-i", str(shmid)], timeout=5).decode()
        nattch_match = None
        import re
        m = re.search(r"nattch\\s*=\\s*(\\d+)", info_out)
        nattch = int(m.group(1)) if m else -1
    except Exception:
        nattch = -1

    addr = libc.shmat(shmid, None, SHM_RDONLY)
    if addr == ctypes.c_void_p(-1).value:
        print(json.dumps({"error": "shmat failed", "key": hex(key)}))
    else:
        # First 4 bytes: used count
        used = struct.unpack_from("<i", (ctypes.c_char * 4).from_address(addr), 0)[0]
        
        ENTRY_SIZE = 176
        RTE_MAX_LCORE = 128
        HEADER_SIZE = 4
        
        entries = []
        for i in range(RTE_MAX_LCORE):
            offset = HEADER_SIZE + i * ENTRY_SIZE
            raw = (ctypes.c_char * ENTRY_SIZE).from_address(addr + offset)
            data = bytes(raw)
            
            hostname = data[0:64].split(b'\\x00', 1)[0].decode('utf-8', errors='replace')
            user = data[64:96].split(b'\\x00', 1)[0].decode('utf-8', errors='replace')
            comm = data[96:160].split(b'\\x00', 1)[0].decode('utf-8', errors='replace')
            pid = struct.unpack_from("<I", data, 164)[0]
            active = struct.unpack_from("<I", data, 172)[0]
            
            if hostname or comm or pid > 0:
                entries.append({
                    "lcore_id": i,
                    "hostname": hostname,
                    "user": user,
                    "comm": comm,
                    "pid": pid,
                    "active": active != 0
                })
        
        libc.shmdt(addr)
        
        stale_warning = None
        if nattch == 0 and len(entries) > 0:
            stale_warning = "nattch=0: no processes attached. Data may be stale (MtlManager may be handling lcore coordination)."
        
        print(json.dumps({
            "shm_exists": True,
            "shm_key": hex(key),
            "used_count": used,
            "entries": entries,
            "nattch": nattch,
            "stale_warning": stale_warning
        }))
__SHM_EOF__`;

    const output = await sshExecSafe(host, script, 15_000);
    if (!output || !output.trim()) {
      return okResponse<MtlLcoreShmData>(
        {
          shm_exists: false,
          shm_key: "unknown",
          used_count: 0,
          entries: [],
          nattch: 0,
          stale_warning: null,
        },
        meta,
      );
    }

    let parsed: Record<string, unknown>;
    try {
      parsed = JSON.parse(output.trim());
    } catch {
      return errorResponse(meta, "SHM_PARSE_ERROR", "Failed to parse SHM reader output");
    }

    if (parsed.error) {
      return okResponse<MtlLcoreShmData>(
        {
          shm_exists: false,
          shm_key: String(parsed.key ?? "unknown"),
          used_count: 0,
          entries: [],
          nattch: 0,
          stale_warning: String(parsed.error),
        },
        meta,
      );
    }

    const entries = (parsed.entries as MtlLcoreShmEntry[]) ?? [];

    return okResponse<MtlLcoreShmData>(
      {
        shm_exists: Boolean(parsed.shm_exists),
        shm_key: String(parsed.shm_key ?? "unknown"),
        used_count: Number(parsed.used_count ?? 0),
        entries,
        nattch: Number(parsed.nattch ?? 0),
        stale_warning: parsed.stale_warning ? String(parsed.stale_warning) : null,
      },
      meta,
    );
  } catch (err) {
    return errorResponse(
      meta,
      "MTL_LCORE_SHM_ERROR",
      `Failed to read lcore SHM: ${err instanceof Error ? err.message : String(err)}`,
      "Ensure Python3 is available on the target host",
    );
  }
}
